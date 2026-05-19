"""
Configuration Loader Module
Loads and manages unified traffic control configuration.
"""

import json
import logging
from pathlib import Path
from typing import Dict, Any, Optional, List
from dataclasses import dataclass

logger = logging.getLogger(__name__)


@dataclass
class TrafficThresholds:
    """Traffic state thresholds."""
    queue_critical: int
    queue_high: int
    queue_medium: int
    queue_low: int
    density_critical_pct: float
    density_high_pct: float
    density_medium_pct: float
    wait_time_critical_sec: float
    wait_time_high_sec: float
    wait_time_medium_sec: float


@dataclass
class NetworkConfig:
    """Network-wide configuration."""
    default_cycle_time_sec: float
    min_phase_duration_sec: float
    max_phase_duration_sec: float
    phase_extension_threshold_sec: float


@dataclass
class EmergencyConfig:
    """Emergency handling configuration."""
    emergency_cycle_time_sec: float
    emergency_max_phase_duration_sec: float
    emergency_min_phase_duration_sec: float
    priority_boost_factor: float


@dataclass
class RLAgentConfig:
    """Reinforcement Learning agent configuration."""
    learning_rate: float
    discount_factor: float
    epsilon_initial: float
    epsilon_final: float
    epsilon_decay: float
    max_queue_penalty: float
    wait_time_penalty_factor: float
    throughput_reward_factor: float
    action_smoothing_factor: float


class TrafficConfigLoader:
    """Load and manage traffic control configuration."""
    
    def __init__(self):
        self.traffic_config = None
        self.phases_config = None
        self.conflicts_config = None
        self._load_all_configs()
    
    def _load_config_file(self, candidate_paths: List[Path], description: str) -> Dict[str, Any]:
        """Load a configuration file from candidate paths."""
        for path in candidate_paths:
            try:
                if not path.exists():
                    continue
                
                with open(path, 'r', encoding='utf-8') as f:
                    config = json.load(f)
                    logger.info(f"Loaded {description} from {path}")
                    return config
            except Exception as e:
                logger.warning(f"Failed to load {description} from {path}: {e}")
                continue
        
        logger.warning(f"No {description} found, using defaults")
        return {}
    
    def _load_all_configs(self):
        """Load all configuration files."""
        # Traffic config
        traffic_candidates = [
            Path("python/server/traffic_config.json"),
            Path("traffic_config.json"),
            Path(__file__).parent / "traffic_config.json",
        ]
        self.traffic_config = self._load_config_file(traffic_candidates, "traffic configuration")
        
        # Traffic phases
        phases_candidates = [
            Path("python/server/traffic_phases.json"),
            Path("traffic_phases.json"),
            Path(__file__).parent / "traffic_phases.json",
        ]
        self.phases_config = self._load_config_file(phases_candidates, "traffic phases configuration")
        
        # Lane conflicts
        conflicts_candidates = [
            Path("python/server/lane_conflicts.json"),
            Path("lane_conflicts.json"),
            Path(__file__).parent / "lane_conflicts.json",
        ]
        self.conflicts_config = self._load_config_file(conflicts_candidates, "lane conflicts configuration")
    
    def get_thresholds(self) -> TrafficThresholds:
        """Get traffic thresholds configuration."""
        thresholds_dict = self.traffic_config.get("thresholds", {})
        return TrafficThresholds(
            queue_critical=thresholds_dict.get("queue_critical", 20),
            queue_high=thresholds_dict.get("queue_high", 15),
            queue_medium=thresholds_dict.get("queue_medium", 8),
            queue_low=thresholds_dict.get("queue_low", 3),
            density_critical_pct=thresholds_dict.get("density_critical_pct", 85.0),
            density_high_pct=thresholds_dict.get("density_high_pct", 70.0),
            density_medium_pct=thresholds_dict.get("density_medium_pct", 50.0),
            wait_time_critical_sec=thresholds_dict.get("wait_time_critical_sec", 60),
            wait_time_high_sec=thresholds_dict.get("wait_time_high_sec", 45),
            wait_time_medium_sec=thresholds_dict.get("wait_time_medium_sec", 25),
        )
    
    def get_network_config(self) -> NetworkConfig:
        """Get network configuration."""
        network_dict = self.traffic_config.get("network", {})
        return NetworkConfig(
            default_cycle_time_sec=network_dict.get("default_cycle_time_sec", 60),
            min_phase_duration_sec=network_dict.get("min_phase_duration_sec", 10),
            max_phase_duration_sec=network_dict.get("max_phase_duration_sec", 90),
            phase_extension_threshold_sec=network_dict.get("phase_extension_threshold_sec", 5),
        )
    
    def get_emergency_config(self) -> EmergencyConfig:
        """Get emergency configuration."""
        emergency_dict = self.traffic_config.get("emergency", {})
        return EmergencyConfig(
            emergency_cycle_time_sec=emergency_dict.get("emergency_cycle_time_sec", 30),
            emergency_max_phase_duration_sec=emergency_dict.get("emergency_max_phase_duration_sec", 120),
            emergency_min_phase_duration_sec=emergency_dict.get("emergency_min_phase_duration_sec", 5),
            priority_boost_factor=emergency_dict.get("priority_boost_factor", 2.0),
        )
    
    def get_rl_agent_config(self) -> RLAgentConfig:
        """Get RL agent configuration."""
        rl_dict = self.traffic_config.get("rl_agent", {})
        return RLAgentConfig(
            learning_rate=rl_dict.get("learning_rate", 0.1),
            discount_factor=rl_dict.get("discount_factor", 0.95),
            epsilon_initial=rl_dict.get("epsilon_initial", 1.0),
            epsilon_final=rl_dict.get("epsilon_final", 0.05),
            epsilon_decay=rl_dict.get("epsilon_decay", 0.9995),
            max_queue_penalty=rl_dict.get("max_queue_penalty", -100.0),
            wait_time_penalty_factor=rl_dict.get("wait_time_penalty_factor", -0.5),
            throughput_reward_factor=rl_dict.get("throughput_reward_factor", 5.0),
            action_smoothing_factor=rl_dict.get("action_smoothing_factor", 0.7),
        )
    
    def get_phase_sequence(self, intersection_id: int) -> List[int]:
        """Get phase sequence for intersection."""
        phases_dict = self.phases_config.get("phases", {})
        default_sequence_name = self.phases_config.get("default_sequence", "standard")
        sequences = self.phases_config.get("phase_sequences", {})
        
        # Return default sequence or standard fallback
        sequence = sequences.get(default_sequence_name, sequences.get("standard", [0, 1]))
        return sequence
    
    def get_lane_conflicts(self, intersection_id: int) -> List[Dict[str, Any]]:
        """Get lane conflicts for intersection."""
        intersections = self.conflicts_config.get("intersections", {})
        intersection_config = intersections.get(str(intersection_id), {})
        return intersection_config.get("conflicts", [])
    
    def get_yellow_duration(self) -> float:
        """Get yellow light duration."""
        return self.phases_config.get("yellow_duration_sec", 3.0)
    
    def get_all_red_duration(self) -> float:
        """Get all-red duration (transition between phases)."""
        return self.phases_config.get("all_red_duration_sec", 2.0)
    
    def get_config_summary(self) -> Dict[str, Any]:
        """Get summary of all loaded configurations."""
        return {
            "traffic_config_loaded": bool(self.traffic_config),
            "phases_config_loaded": bool(self.phases_config),
            "conflicts_config_loaded": bool(self.conflicts_config),
            "thresholds": {
                "queue_critical": self.get_thresholds().queue_critical,
                "density_critical_pct": self.get_thresholds().density_critical_pct,
                "wait_time_critical_sec": self.get_thresholds().wait_time_critical_sec,
            },
            "network": {
                "default_cycle_time_sec": self.get_network_config().default_cycle_time_sec,
                "min_phase_duration_sec": self.get_network_config().min_phase_duration_sec,
                "max_phase_duration_sec": self.get_network_config().max_phase_duration_sec,
            },
            "emergency": {
                "emergency_cycle_time_sec": self.get_emergency_config().emergency_cycle_time_sec,
                "priority_boost_factor": self.get_emergency_config().priority_boost_factor,
            },
            "rl_agent": {
                "learning_rate": self.get_rl_agent_config().learning_rate,
                "epsilon_decay": self.get_rl_agent_config().epsilon_decay,
                "discount_factor": self.get_rl_agent_config().discount_factor,
            },
        }


# Global config instance
_config_loader = None


def get_config_loader() -> TrafficConfigLoader:
    """Get global config loader instance."""
    global _config_loader
    if _config_loader is None:
        _config_loader = TrafficConfigLoader()
    return _config_loader


def reload_config():
    """Reload configuration (useful for runtime updates)."""
    global _config_loader
    _config_loader = TrafficConfigLoader()
    return _config_loader
