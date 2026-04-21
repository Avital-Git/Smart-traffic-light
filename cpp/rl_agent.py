"""
rl_agent.py
-----------
אלגוריתם למידת חיזוק עמוקה מרובת סוכנים (Multi-Agent Deep RL)
עם אתחול מדיניות מבוססת חוקים (Rule-Based Policy Initialization).

אביטל חדד | מכללת בנות בת שבע
פרויקט גמר — מערכת ניהול תנועה חכמה

══════════════════════════════════════════════════════════════
ארכיטקטורה
══════════════════════════════════════════════════════════════

כל צומת היא סוכן עצמאי (Independent Agent) עם רשת נוירונים משלה.

State vector של סוכן (צומת) כולל:
  ┌─────────────────────────────────────────────────────────┐
  │  Local State      │ (4 * num_lanes + 1) ערכים          │
  │                   │ V, P, D, W לכל נתיב + emergency    │
  ├───────────────────┼─────────────────────────────────────┤
  │  Neighbor States  │ לכל שכן: total_vehicles,           │
  │                   │ max_density, avg_waiting,           │
  │                   │ emergency_active                    │
  └─────────────────────────────────────────────────────────┘

Action = באיזה נתיב לתת ירוק (0..num_lanes-1) או Hold (-1)

Policy Initialization:
  הרשת מאותחלת ע"י pre-training על החלטות של ה-Rule-Based policy,
  כך שנקודת הפתיחה כבר סבירה לפני שה-RL מתחיל ללמוד.

Reward:
  ⚠ פונקציית התגמול מוגדרת כ-placeholder — יש להתאים אותה.
  המבנה מוכן: compute_reward() מקבלת prev_state, curr_state, action
  ומחזירה float. הכללים הקבועים:
    — תגמול חירום = הגבוה ביותר (עדיפות עליונה)
    — שאר הפרמטרים: פתוח להגדרה

תלויות:
    pip install torch numpy
"""

from __future__ import annotations

import math
import random
import time
import json
import copy
from collections import deque
from dataclasses import dataclass, field, asdict
from typing import Dict, List, Optional, Tuple

import numpy as np

# ─── Optional: PyTorch for Deep RL ───
try:
    import torch
    import torch.nn as nn
    import torch.optim as optim
    TORCH_AVAILABLE = True
except ImportError:
    TORCH_AVAILABLE = False
    print("[RL] ⚠ PyTorch לא מותקן — ישתמש ב-Tabular Q-Learning בלבד.")
    print("[RL]   להתקנה: pip install torch")


# ══════════════════════════════════════════════════════
# 1. מבני נתונים
# ══════════════════════════════════════════════════════

@dataclass
class NeighborSummary:
    """
    סיכום מצב של צומת שכנה — נשלח כחלק מה-State.
    תקשורת בין סוכנים: כל סוכן מקבל סיכום השכנים שלו.
    """
    intersection_id: int
    total_vehicles: int = 0
    max_density_pct: float = 0.0
    avg_waiting_sec: float = 0.0
    emergency_active: bool = False

    def to_vector(self) -> List[float]:
        """4 ערכים לכל שכן."""
        return [
            float(self.total_vehicles) / 50.0,          # normalize
            self.max_density_pct / 100.0,
            min(self.avg_waiting_sec / 60.0, 1.0),
            1.0 if self.emergency_active else 0.0,
        ]


@dataclass
class AgentState:
    """
    המצב המלא שסוכן רואה — כולל מידע מקומי + שכנים.
    """
    intersection_id: int
    num_lanes: int
    timestamp: float
    # local
    vehicle_counts: List[int] = field(default_factory=list)
    pedestrian_counts: List[int] = field(default_factory=list)
    density_pcts: List[float] = field(default_factory=list)
    waiting_times: List[float] = field(default_factory=list)
    emergency_active: bool = False
    emergency_lane_id: int = -1
    # neighbors
    neighbor_summaries: List[NeighborSummary] = field(default_factory=list)

    @property
    def local_vector_size(self) -> int:
        return 4 * self.num_lanes + 1

    @property
    def neighbor_vector_size(self) -> int:
        """4 ערכים לכל שכן, עד MAX_NEIGHBORS."""
        return 4 * MAX_NEIGHBORS

    @property
    def total_vector_size(self) -> int:
        return self.local_vector_size + self.neighbor_vector_size

    def to_local_vector(self) -> List[float]:
        """וקטור מקומי: (4*num_lanes + 1)."""
        vec = []
        for v in self.vehicle_counts:
            vec.append(float(v) / 15.0)       # normalize
        for p in self.pedestrian_counts:
            vec.append(float(p) / 10.0)
        for d in self.density_pcts:
            vec.append(d / 100.0)
        for w in self.waiting_times:
            vec.append(min(w / 60.0, 1.0))
        vec.append(1.0 if self.emergency_active else 0.0)
        return vec

    def to_neighbor_vector(self) -> List[float]:
        """וקטור שכנים: (4 * MAX_NEIGHBORS), padded with zeros."""
        vec = []
        for i in range(MAX_NEIGHBORS):
            if i < len(self.neighbor_summaries):
                vec.extend(self.neighbor_summaries[i].to_vector())
            else:
                vec.extend([0.0, 0.0, 0.0, 0.0])
        return vec

    def to_full_vector(self) -> List[float]:
        """וקטור מלא: local + neighbors."""
        return self.to_local_vector() + self.to_neighbor_vector()

    def to_tensor(self) -> "torch.Tensor":
        """ממיר ל-PyTorch tensor."""
        return torch.FloatTensor(self.to_full_vector()).unsqueeze(0)

    @property
    def total_vehicles(self) -> int:
        return sum(self.vehicle_counts)

    @property
    def max_density(self) -> float:
        return max(self.density_pcts) if self.density_pcts else 0.0

    @property
    def avg_waiting(self) -> float:
        return sum(self.waiting_times) / len(self.waiting_times) if self.waiting_times else 0.0


# מקסימום שכנים שנתמכים בוקטור (padding)
MAX_NEIGHBORS = 4


# ══════════════════════════════════════════════════════
# 2. פונקציית תגמול — REWARD
# ══════════════════════════════════════════════════════

class RewardFunction:
    """
    מחלקת תגמול — מגדירה את ה-Reward עבור כל צעד.

    ⚠ פונקציה זו פתוחה להתאמה! ⚠

    הכלל הקבוע היחיד:
        מענה לקריאת חירום = התגמול הגבוה ביותר.

    כל שאר הפרמטרים מוגדרים כ-placeholders עם ערכי ברירת מחדל
    שניתן לשנות. כל weight שמסומן ב-TODO הוא פרמטר שצריך לכוונן.
    """

    def __init__(self):
        # ═══ משקלות תגמול — TODO: לכוונן! ═══

        # חירום — תגמול חיובי על מענה נכון (הגבוה ביותר!)
        self.emergency_correct_reward: float = 0.0    # TODO: הגדירי ערך
        # חירום — עונש על אי-מענה כשיש חירום
        self.emergency_missed_penalty: float = 0.0    # TODO: הגדירי ערך

        # זמן המתנה — עונש על המתנה ארוכה
        self.waiting_time_penalty_weight: float = 0.0  # TODO: הגדירי ערך

        # צפיפות — עונש על עומס גבוה
        self.density_penalty_weight: float = 0.0       # TODO: הגדירי ערך

        # תפוקה — תגמול על הפחתת רכבים (רכבים שעברו)
        self.throughput_reward_weight: float = 0.0     # TODO: הגדירי ערך

        # הולכי רגל — עונש על המתנה ארוכה של הולכי רגל
        self.pedestrian_wait_penalty: float = 0.0      # TODO: הגדירי ערך

        # שכנים — עונש על גרימת עומס לשכנים (מניעת פקק שרשרת)
        self.neighbor_congestion_penalty: float = 0.0  # TODO: הגדירי ערך

        # הוגנות — עונש על הרעבת נתיב (נתיב שממתין יותר מדי)
        self.starvation_penalty_threshold_sec: float = 0.0  # TODO: סף בשניות
        self.starvation_penalty_weight: float = 0.0         # TODO: הגדירי ערך

        # בונוס רציפות — תגמול על "גל ירוק" בין צמתים
        self.green_wave_bonus: float = 0.0             # TODO: הגדירי ערך

    def compute_reward(
        self,
        prev_state: AgentState,
        curr_state: AgentState,
        action: int,
    ) -> float:
        """
        חישוב התגמול עבור מעבר (prev_state, action) → curr_state.

        Parameters
        ----------
        prev_state : AgentState
            המצב לפני הפעולה.
        curr_state : AgentState
            המצב אחרי הפעולה.
        action : int
            הנתיב שקיבל ירוק (-1 = Hold).

        Returns
        -------
        float
            ערך התגמול.
        """
        reward = 0.0

        # ──────────────────────────────────
        # 1. חירום — עדיפות עליונה!
        # ──────────────────────────────────
        if prev_state.emergency_active:
            if action == prev_state.emergency_lane_id:
                reward += self.emergency_correct_reward
            else:
                reward -= self.emergency_missed_penalty

        # ──────────────────────────────────
        # 2. זמן המתנה
        # ──────────────────────────────────
        total_waiting = sum(curr_state.waiting_times)
        reward -= total_waiting * self.waiting_time_penalty_weight

        # ──────────────────────────────────
        # 3. צפיפות
        # ──────────────────────────────────
        avg_density = (
            sum(curr_state.density_pcts) / len(curr_state.density_pcts)
            if curr_state.density_pcts else 0.0
        )
        reward -= avg_density * self.density_penalty_weight

        # ──────────────────────────────────
        # 4. תפוקה — הפחתת רכבים
        # ──────────────────────────────────
        vehicles_cleared = prev_state.total_vehicles - curr_state.total_vehicles
        reward += vehicles_cleared * self.throughput_reward_weight

        # ──────────────────────────────────
        # 5. הולכי רגל
        # ──────────────────────────────────
        total_ped = sum(curr_state.pedestrian_counts)
        reward -= total_ped * self.pedestrian_wait_penalty

        # ──────────────────────────────────
        # 6. עומס שכנים (מניעת פקק שרשרת)
        # ──────────────────────────────────
        for ns in curr_state.neighbor_summaries:
            reward -= (ns.max_density_pct / 100.0) * self.neighbor_congestion_penalty

        # ──────────────────────────────────
        # 7. הוגנות — מניעת הרעבה
        # ──────────────────────────────────
        if self.starvation_penalty_threshold_sec > 0:
            for w in curr_state.waiting_times:
                if w > self.starvation_penalty_threshold_sec:
                    reward -= self.starvation_penalty_weight

        # ──────────────────────────────────
        # 8. בונוס "גל ירוק"
        # ──────────────────────────────────
        # אם שכנים נתנו ירוק לכיוון שלנו → תגמול
        # (ניתן להרחיב בעתיד)

        return reward


# ══════════════════════════════════════════════════════
# 3. Rule-Based Policy (מדיניות חוקים)
# ══════════════════════════════════════════════════════

class RuleBasedPolicy:
    """
    מדיניות מבוססת חוקים — משמשת כנקודת פתיחה (Policy Initialization)
    לפני שה-RL מתחיל ללמוד.

    חוקים:
    1. חירום → ירוק מיידי לנתיב החירום
    2. אחרת → בחירת הנתיב עם הציון הגבוה ביותר:
       score = vehicles * w1 + density * w2 + waiting * w3
    3. התחשבות בשכנים: אם שכן עמוס → הקטנת הציון לנתיב שמוביל אליו
    """

    def __init__(self, vehicle_w: float = 1.5, density_w: float = 0.5,
                 waiting_w: float = 0.2, neighbor_penalty: float = 0.3):
        self.vehicle_w = vehicle_w
        self.density_w = density_w
        self.waiting_w = waiting_w
        self.neighbor_penalty = neighbor_penalty

    def select_action(self, state: AgentState) -> int:
        """בוחר נתיב (action) לפי חוקים."""
        # חירום — עדיפות מיידית!
        if state.emergency_active and state.emergency_lane_id >= 0:
            return state.emergency_lane_id

        # חישוב ציון לכל נתיב
        scores = []
        for i in range(state.num_lanes):
            score = (
                state.vehicle_counts[i] * self.vehicle_w
                + state.density_pcts[i] * self.density_w
                + state.waiting_times[i] * self.waiting_w
            )
            scores.append(score)

        # הפחתת ציון אם שכן עמוס (מניעת פקק שרשרת)
        for ns in state.neighbor_summaries:
            if ns.max_density_pct > 70:
                # הנתיב הראשון "משלם" את הקנס (פישוט)
                # בגרסה מתקדמת — להתאים לפי כיוון השכן
                if scores:
                    scores[0] -= self.neighbor_penalty * ns.max_density_pct

        if not scores:
            return 0

        return int(np.argmax(scores))

    def generate_training_data(
        self, states: List[AgentState]
    ) -> List[Tuple[List[float], int]]:
        """
        מייצר מידע אימון (state_vector, action) מתוך Rule-Based,
        לצורך Pre-Training של הרשת.
        """
        data = []
        for state in states:
            action = self.select_action(state)
            vector = state.to_full_vector()
            data.append((vector, action))
        return data


# ══════════════════════════════════════════════════════
# 4. Deep Q-Network (DQN)
# ══════════════════════════════════════════════════════

if TORCH_AVAILABLE:

    class DQNetwork(nn.Module):
        """
        רשת Deep Q-Network (DQN) לסוכן צומת.

        קלט: state vector (local + neighbors)
        פלט: Q-value לכל action (Hold + Green0..Green{n-1})
        """

        def __init__(self, input_size: int, num_actions: int, hidden_size: int = 128):
            super().__init__()
            self.net = nn.Sequential(
                nn.Linear(input_size, hidden_size),
                nn.ReLU(),
                nn.Linear(hidden_size, hidden_size),
                nn.ReLU(),
                nn.Linear(hidden_size, hidden_size // 2),
                nn.ReLU(),
                nn.Linear(hidden_size // 2, num_actions),
            )

        def forward(self, x: torch.Tensor) -> torch.Tensor:
            return self.net(x)


    class ReplayBuffer:
        """Experience Replay Buffer — זיכרון חוויות לאימון DQN."""

        def __init__(self, capacity: int = 10000):
            self.buffer: deque = deque(maxlen=capacity)

        def push(self, state_vec: List[float], action: int,
                 reward: float, next_state_vec: List[float], done: bool):
            self.buffer.append((state_vec, action, reward, next_state_vec, done))

        def sample(self, batch_size: int) -> Tuple:
            batch = random.sample(self.buffer, min(batch_size, len(self.buffer)))
            states, actions, rewards, next_states, dones = zip(*batch)
            return (
                torch.FloatTensor(np.array(states)),
                torch.LongTensor(actions),
                torch.FloatTensor(rewards),
                torch.FloatTensor(np.array(next_states)),
                torch.FloatTensor(dones),
            )

        def __len__(self):
            return len(self.buffer)


# ══════════════════════════════════════════════════════
# 5. סוכן DQN בודד (Single Agent)
# ══════════════════════════════════════════════════════

class IntersectionAgent:
    """
    סוכן למידת חיזוק עמוקה עבור צומת אחת.

    מאפשר:
    - אתחול מדיניות מ-Rule-Based (policy initialization)
    - למידה מחוויה (experience replay + DQN)
    - תקשורת עם שכנים דרך ה-state vector
    """

    def __init__(
        self,
        intersection_id: int,
        num_lanes: int,
        neighbor_ids: List[int] = None,
        learning_rate: float = 1e-3,
        gamma: float = 0.95,
        epsilon_start: float = 0.3,
        epsilon_end: float = 0.05,
        epsilon_decay: float = 0.995,
        buffer_capacity: int = 10000,
        batch_size: int = 64,
        target_update_freq: int = 50,
        hidden_size: int = 128,
    ):
        self.intersection_id = intersection_id
        self.num_lanes = num_lanes
        self.neighbor_ids = neighbor_ids or []
        self.num_actions = num_lanes + 1  # Hold + Green0..Green(n-1)
        self.gamma = gamma
        self.epsilon = epsilon_start
        self.epsilon_end = epsilon_end
        self.epsilon_decay = epsilon_decay
        self.batch_size = batch_size
        self.target_update_freq = target_update_freq
        self.step_count = 0
        self.training_enabled = True

        # Rule-Based policy — תמיד זמינה כ-fallback
        self.rule_policy = RuleBasedPolicy()

        # Reward function
        self.reward_fn = RewardFunction()

        # Deep RL — רק אם PyTorch זמין
        self.use_deep = TORCH_AVAILABLE
        if self.use_deep:
            # חישוב גודל קלט
            self.input_size = (4 * num_lanes + 1) + (4 * MAX_NEIGHBORS)

            # רשתות Q: policy network + target network
            self.q_net = DQNetwork(self.input_size, self.num_actions, hidden_size)
            self.target_net = DQNetwork(self.input_size, self.num_actions, hidden_size)
            self.target_net.load_state_dict(self.q_net.state_dict())
            self.target_net.eval()

            self.optimizer = optim.Adam(self.q_net.parameters(), lr=learning_rate)
            self.replay_buffer = ReplayBuffer(buffer_capacity)

            print(
                f"[Agent #{intersection_id}] DQN created | "
                f"input={self.input_size} | actions={self.num_actions} | "
                f"hidden={hidden_size}"
            )
        else:
            self.q_table: Dict[str, Dict[int, float]] = {}
            print(f"[Agent #{intersection_id}] Tabular Q-Learning (no PyTorch)")

        # מצב קודם (לצורך חישוב reward)
        self.prev_state: Optional[AgentState] = None
        self.prev_action: Optional[int] = None

    # ──────────────────────────────────────────────
    # Policy Initialization — אתחול מ-Rule-Based
    # ──────────────────────────────────────────────

    def initialize_from_rule_based(self, num_samples: int = 1000):
        """
        Pre-Training: מאמנת את הרשת על החלטות ה-Rule-Based,
        כך שנקודת הפתיחה כבר סבירה.
        """
        if not self.use_deep:
            print(f"[Agent #{self.intersection_id}] Policy init: Rule-Based (no PyTorch)")
            return

        print(f"[Agent #{self.intersection_id}] Policy Initialization — "
              f"Pre-training on {num_samples} rule-based samples...")

        # ייצור מצבים אקראיים
        synthetic_states = []
        for _ in range(num_samples):
            state = self._generate_random_state()
            synthetic_states.append(state)

        # ייצור זוגות (vector, action) מ-Rule-Based
        training_data = self.rule_policy.generate_training_data(synthetic_states)

        # אימון הרשת בפיקוח (Supervised Learning על Rule-Based)
        self.q_net.train()
        criterion = nn.CrossEntropyLoss()
        optimizer = optim.Adam(self.q_net.parameters(), lr=1e-3)

        vectors = torch.FloatTensor(np.array([d[0] for d in training_data]))
        actions = torch.LongTensor([d[1] for d in training_data])

        for epoch in range(30):
            # shuffle
            perm = torch.randperm(len(training_data))
            vectors_shuffled = vectors[perm]
            actions_shuffled = actions[perm]

            total_loss = 0.0
            for i in range(0, len(training_data), self.batch_size):
                batch_v = vectors_shuffled[i:i + self.batch_size]
                batch_a = actions_shuffled[i:i + self.batch_size]

                q_values = self.q_net(batch_v)
                loss = criterion(q_values, batch_a)

                optimizer.zero_grad()
                loss.backward()
                optimizer.step()
                total_loss += loss.item()

            if (epoch + 1) % 10 == 0:
                print(f"  Epoch {epoch+1}/30 | Loss: {total_loss:.4f}")

        # עדכון target network
        self.target_net.load_state_dict(self.q_net.state_dict())
        print(f"[Agent #{self.intersection_id}] Policy initialization complete ✅")

    def _generate_random_state(self) -> AgentState:
        """מייצרת מצב אקראי לצורך pre-training."""
        has_emergency = random.random() < 0.1
        state = AgentState(
            intersection_id=self.intersection_id,
            num_lanes=self.num_lanes,
            timestamp=time.time(),
            vehicle_counts=[random.randint(0, 15) for _ in range(self.num_lanes)],
            pedestrian_counts=[random.randint(0, 5) for _ in range(self.num_lanes)],
            density_pcts=[random.uniform(0, 100) for _ in range(self.num_lanes)],
            waiting_times=[random.uniform(0, 60) for _ in range(self.num_lanes)],
            emergency_active=has_emergency,
            emergency_lane_id=random.randint(0, self.num_lanes - 1) if has_emergency else -1,
            neighbor_summaries=[
                NeighborSummary(
                    intersection_id=nid,
                    total_vehicles=random.randint(0, 40),
                    max_density_pct=random.uniform(0, 100),
                    avg_waiting_sec=random.uniform(0, 40),
                    emergency_active=random.random() < 0.05,
                )
                for nid in self.neighbor_ids
            ],
        )
        return state

    # ──────────────────────────────────────────────
    # בחירת פעולה (Action Selection)
    # ──────────────────────────────────────────────

    def select_action(self, state: AgentState) -> int:
        """
        בחירת פעולה — epsilon-greedy עם fallback ל-Rule-Based.

        Returns: action index (0 = Green lane 0, ..., n-1 = Green lane n-1, n = Hold)
        """
        # חירום — תמיד Rule-Based (אמינות מלאה!)
        if state.emergency_active and state.emergency_lane_id >= 0:
            return state.emergency_lane_id

        # Exploration (epsilon-greedy)
        if random.random() < self.epsilon:
            return random.randint(0, self.num_actions - 1)

        # Exploitation
        if self.use_deep:
            self.q_net.eval()
            with torch.no_grad():
                state_tensor = torch.FloatTensor(state.to_full_vector()).unsqueeze(0)
                q_values = self.q_net(state_tensor)
                return q_values.argmax(dim=1).item()
        else:
            # Tabular fallback
            key = self._state_key(state)
            if key in self.q_table:
                return max(self.q_table[key], key=self.q_table[key].get)
            return self.rule_policy.select_action(state)

    def action_to_string(self, action: int) -> str:
        """ממיר action index למחרוזת."""
        if action >= self.num_lanes or action < 0:
            return "Hold"
        return f"Green{action}"

    # ──────────────────────────────────────────────
    # צעד למידה (Learning Step)
    # ──────────────────────────────────────────────

    def step(self, curr_state: AgentState) -> Tuple[int, float]:
        """
        צעד אחד: בוחר פעולה, מחשב reward מהצעד הקודם, מעדכן.

        Returns: (action, reward)
            reward הוא מהצעד הקודם (0.0 בצעד הראשון).
        """
        reward = 0.0

        # חישוב reward מהצעד הקודם
        if self.prev_state is not None and self.prev_action is not None:
            reward = self.reward_fn.compute_reward(
                self.prev_state, curr_state, self.prev_action
            )
            # שמירה ב-Replay Buffer
            self._store_experience(
                self.prev_state, self.prev_action, reward, curr_state, done=False
            )
            # אימון
            if self.training_enabled:
                self._train_step()

        # בחירת פעולה חדשה
        action = self.select_action(curr_state)

        # עדכון epsilon
        self.epsilon = max(self.epsilon_end, self.epsilon * self.epsilon_decay)

        # שמירת מצב לצעד הבא
        self.prev_state = copy.deepcopy(curr_state)
        self.prev_action = action

        self.step_count += 1
        return action, reward

    def _store_experience(self, state: AgentState, action: int,
                          reward: float, next_state: AgentState, done: bool):
        """שמירת חוויה ב-buffer."""
        if self.use_deep:
            self.replay_buffer.push(
                state.to_full_vector(), action,
                reward, next_state.to_full_vector(), done
            )
        else:
            key = self._state_key(state)
            next_key = self._state_key(next_state)
            # Tabular Q-update
            old_q = self.q_table.get(key, {}).get(action, 0.0)
            next_max = max(self.q_table.get(next_key, {0: 0.0}).values())
            new_q = old_q + 0.1 * (reward + self.gamma * next_max - old_q)
            if key not in self.q_table:
                self.q_table[key] = {}
            self.q_table[key][action] = new_q

    def _train_step(self):
        """צעד אימון אחד של DQN."""
        if not self.use_deep:
            return
        if len(self.replay_buffer) < self.batch_size:
            return

        states, actions, rewards, next_states, dones = self.replay_buffer.sample(
            self.batch_size
        )

        self.q_net.train()

        # Q(s, a) — הערכים הנוכחיים
        q_values = self.q_net(states).gather(1, actions.unsqueeze(1)).squeeze(1)

        # max Q'(s', a') — מה-target network
        with torch.no_grad():
            next_q = self.target_net(next_states).max(dim=1)[0]
            target = rewards + self.gamma * next_q * (1 - dones)

        loss = nn.MSELoss()(q_values, target)

        self.optimizer.zero_grad()
        loss.backward()
        # gradient clipping למניעת פיצוצי gradients
        nn.utils.clip_grad_norm_(self.q_net.parameters(), max_norm=1.0)
        self.optimizer.step()

        # עדכון target network
        if self.step_count % self.target_update_freq == 0:
            self.target_net.load_state_dict(self.q_net.state_dict())

    def _state_key(self, state: AgentState) -> str:
        """מפתח מצב ל-Tabular Q-Learning."""
        parts = []
        for v in state.vehicle_counts:
            parts.append(str(min(v // 4, 2)))
        parts.append("E" if state.emergency_active else "_")
        for ns in state.neighbor_summaries:
            parts.append(str(min(ns.total_vehicles // 10, 3)))
        return ",".join(parts)

    # ──────────────────────────────────────────────
    # שמירה / טעינה
    # ──────────────────────────────────────────────

    def save(self, path: str):
        """שמירת המודל לקובץ."""
        if self.use_deep:
            torch.save({
                "q_net": self.q_net.state_dict(),
                "target_net": self.target_net.state_dict(),
                "optimizer": self.optimizer.state_dict(),
                "epsilon": self.epsilon,
                "step_count": self.step_count,
            }, path)
        else:
            import pickle
            with open(path, "wb") as f:
                pickle.dump({"q_table": self.q_table, "epsilon": self.epsilon}, f)
        print(f"[Agent #{self.intersection_id}] Model saved → {path}")

    def load(self, path: str):
        """טעינת מודל מקובץ."""
        if self.use_deep:
            checkpoint = torch.load(path, weights_only=False)
            self.q_net.load_state_dict(checkpoint["q_net"])
            self.target_net.load_state_dict(checkpoint["target_net"])
            self.optimizer.load_state_dict(checkpoint["optimizer"])
            self.epsilon = checkpoint["epsilon"]
            self.step_count = checkpoint["step_count"]
        else:
            import pickle
            with open(path, "rb") as f:
                data = pickle.load(f)
            self.q_table = data["q_table"]
            self.epsilon = data["epsilon"]
        print(f"[Agent #{self.intersection_id}] Model loaded ← {path}")


# ══════════════════════════════════════════════════════
# 6. Multi-Agent Controller (בקר מרובה סוכנים)
# ══════════════════════════════════════════════════════

class MultiAgentController:
    """
    בקר מרכזי למערכת מרובת סוכנים.

    אחריות:
    - ניהול סוכן לכל צומת
    - תקשורת בין סוכנים (שליחת NeighborSummary)
    - Policy Initialization ע"י Rule-Based
    - אימון ה-DQN

    תקשורת בין צמתים:
    ─────────────────
    בכל צעד, כל סוכן מקבל state שכולל NeighborSummary
    של כל הצמתים השכנות שלו. כך הסוכן מקבל החלטות
    בהתאם למצב הכולל ולא רק למצב המקומי.
    """

    def __init__(self):
        self.agents: Dict[int, IntersectionAgent] = {}
        self.neighbor_map: Dict[int, List[int]] = {}  # intersection_id → [neighbor_ids]
        self.last_states: Dict[int, AgentState] = {}   # מצב אחרון לכל צומת
        self.reward_fn = RewardFunction()

    def register_intersection(
        self,
        intersection_id: int,
        num_lanes: int,
        neighbor_ids: List[int] = None,
        **agent_kwargs,
    ):
        """רישום צומת חדשה — יוצרת סוכן."""
        neighbor_ids = neighbor_ids or []
        self.neighbor_map[intersection_id] = neighbor_ids

        agent = IntersectionAgent(
            intersection_id=intersection_id,
            num_lanes=num_lanes,
            neighbor_ids=neighbor_ids,
            **agent_kwargs,
        )
        # שיתוף פונקציית reward אחידה
        agent.reward_fn = self.reward_fn
        self.agents[intersection_id] = agent

        print(
            f"[Controller] Registered intersection #{intersection_id} | "
            f"{num_lanes} lanes | neighbors={neighbor_ids}"
        )

    def initialize_all_from_rule_based(self, num_samples: int = 1000):
        """
        Policy Initialization לכל הסוכנים.
        מאמנת כל רשת על החלטות Rule-Based.
        """
        print("═" * 60)
        print("  Policy Initialization — Pre-training from Rule-Based")
        print("═" * 60)
        for agent in self.agents.values():
            agent.initialize_from_rule_based(num_samples)
        print("═" * 60)
        print("  Policy Initialization Complete ✅")
        print("═" * 60)

    def build_agent_state(
        self,
        intersection_id: int,
        local_data: dict,
    ) -> AgentState:
        """
        בונה AgentState מלא כולל NeighborSummary מהמצבים האחרונים.

        כאן מתרחשת התקשורת בין הצמתים:
        הסוכן מקבל סיכום מצב של כל שכן.

        Parameters
        ----------
        intersection_id : int
            מזהה הצומת.
        local_data : dict
            נתונים מקומיים מהמצלמה/סימולציה:
            {
                "vehicle_counts": [int, ...],
                "pedestrian_counts": [int, ...],
                "density_pcts": [float, ...],
                "waiting_times": [float, ...],
                "emergency_active": bool,
                "emergency_lane_id": int,
            }
        """
        agent = self.agents.get(intersection_id)
        if not agent:
            raise ValueError(f"Intersection #{intersection_id} not registered")

        # בניית סיכום שכנים מהמצבים האחרונים
        neighbor_summaries = []
        for nid in self.neighbor_map.get(intersection_id, []):
            ns = self.last_states.get(nid)
            if ns:
                neighbor_summaries.append(NeighborSummary(
                    intersection_id=nid,
                    total_vehicles=ns.total_vehicles,
                    max_density_pct=ns.max_density,
                    avg_waiting_sec=ns.avg_waiting,
                    emergency_active=ns.emergency_active,
                ))
            else:
                neighbor_summaries.append(NeighborSummary(intersection_id=nid))

        state = AgentState(
            intersection_id=intersection_id,
            num_lanes=agent.num_lanes,
            timestamp=time.time(),
            vehicle_counts=local_data.get("vehicle_counts", [0] * agent.num_lanes),
            pedestrian_counts=local_data.get("pedestrian_counts", [0] * agent.num_lanes),
            density_pcts=local_data.get("density_pcts", [0.0] * agent.num_lanes),
            waiting_times=local_data.get("waiting_times", [0.0] * agent.num_lanes),
            emergency_active=local_data.get("emergency_active", False),
            emergency_lane_id=local_data.get("emergency_lane_id", -1),
            neighbor_summaries=neighbor_summaries,
        )

        # שמירת מצב לתקשורת עם שכנים
        self.last_states[intersection_id] = state
        return state

    def step(self, intersection_id: int, local_data: dict) -> Tuple[str, float]:
        """
        צעד אחד עבור צומת — בונה state, בוחר action, מעדכן.

        Returns: (action_string, reward)
        """
        agent = self.agents.get(intersection_id)
        if not agent:
            return "Hold", 0.0

        state = self.build_agent_state(intersection_id, local_data)
        action, reward = agent.step(state)
        action_str = agent.action_to_string(action)
        return action_str, reward

    def select_action(self, intersection_id: int, local_data: dict) -> str:
        """בחירת פעולה בלבד (בלי למידה) — לשימוש ב-inference."""
        agent = self.agents.get(intersection_id)
        if not agent:
            return "Hold"
        state = self.build_agent_state(intersection_id, local_data)
        action = agent.select_action(state)
        return agent.action_to_string(action)

    def set_training(self, enabled: bool):
        """הפעלה/כיבוי מצב אימון."""
        for agent in self.agents.values():
            agent.training_enabled = enabled

    def save_all(self, directory: str = "models"):
        """שמירת כל המודלים."""
        import os
        os.makedirs(directory, exist_ok=True)
        for iid, agent in self.agents.items():
            path = os.path.join(directory, f"agent_{iid}.pt")
            agent.save(path)

    def load_all(self, directory: str = "models"):
        """טעינת כל המודלים."""
        import os
        for iid, agent in self.agents.items():
            path = os.path.join(directory, f"agent_{iid}.pt")
            if os.path.exists(path):
                agent.load(path)

    def get_stats(self) -> dict:
        """סטטיסטיקות של כל הסוכנים."""
        return {
            iid: {
                "step_count": agent.step_count,
                "epsilon": round(agent.epsilon, 4),
                "buffer_size": (
                    len(agent.replay_buffer) if agent.use_deep
                    else len(agent.q_table)
                ),
            }
            for iid, agent in self.agents.items()
        }


# ══════════════════════════════════════════════════════
# 7. בדיקה עצמית
# ══════════════════════════════════════════════════════

if __name__ == "__main__":
    print("═" * 60)
    print("  🚦 Multi-Agent Deep RL — Self Test")
    print("═" * 60)

    # יצירת בקר
    controller = MultiAgentController()

    # רישום צמתים עם שכנים (כמו ב-DB)
    controller.register_intersection(1, num_lanes=4, neighbor_ids=[2, 3, 4])
    controller.register_intersection(2, num_lanes=3, neighbor_ids=[1])
    controller.register_intersection(3, num_lanes=6, neighbor_ids=[1])
    controller.register_intersection(4, num_lanes=2, neighbor_ids=[1])

    # Policy Initialization — pre-training מ-Rule-Based
    controller.initialize_all_from_rule_based(num_samples=500)

    # סימולציית 50 צעדים
    print("\n=== Simulation (50 steps) ===\n")
    for step_i in range(50):
        for iid in [1, 2, 3, 4]:
            agent = controller.agents[iid]
            n = agent.num_lanes
            local_data = {
                "vehicle_counts": [random.randint(0, 12) for _ in range(n)],
                "pedestrian_counts": [random.randint(0, 4) for _ in range(n)],
                "density_pcts": [random.uniform(0, 100) for _ in range(n)],
                "waiting_times": [random.uniform(0, 30) for _ in range(n)],
                "emergency_active": random.random() < 0.05,
                "emergency_lane_id": random.randint(0, n - 1),
            }
            action_str, reward = controller.step(iid, local_data)

            if step_i % 10 == 0 and iid == 1:
                print(f"  Step {step_i} | Int#{iid} → {action_str} | reward={reward:.2f}")

    print("\n=== Stats ===")
    for iid, stats in controller.get_stats().items():
        print(f"  Agent #{iid}: {stats}")

    print("\n✅ Self test complete!")
