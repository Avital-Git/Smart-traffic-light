#pragma once

#include "Junction.h"
#include "ThresholdConfig.h"
#include "NeighborCoordConfig.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <random>
#include <cstdint>

namespace traffic {

struct RLConfig {
    double alpha = 0.10;// קצב הלמידה
    double gamma = 0.95;//פקטור היוון - התחשבות עתידנית
    double epsilon = 0.15;//סיכוי לבחירה אקראית לעומת בחית פעולה טובה שהוא כבר מכיר
    double epsilonMin = 0.02;//סיכוי מינימלי לבחירה אקראית - אף פעם הסוכן לא יפסיק לחקור אפשרויות חדשות
    double epsilonDecay = 0.995;//קצב הדעיכה של האפסילון  
};

class RLAgent {
public:
    explicit RLAgent(//כדי שלא יהיה אפשר לשנות ערכי פרמטר מאחורי הקלעים
        RLConfig cfg = {},//הגדרות למידה
        TrafficThresholdConfig thresholds = {},//ספי תנועה
        NeighborCoordConfig neighborConfig = {}//חוקי התיאום והמרחקים עם צמתים שכנים
    );

    // Choose next action for a junction state + available phases
    int selectAction(
        const JunctionState& state,//מצב הצומת הנוכחי
        const std::vector<Action>& validActions,// פעולות חוקיות שניתן לבצע
        std::optional<int> emergencyPhase//אם יש מצב חירום, איזה פאזה תעדיף לתת לו עדיפות
    );

    // Q-learning update
    void update(
        const JunctionState& prevState,//מצב הצומת לפני הפעולה
        int actionPhaseId,//ההפאזה שנבחרה
        double reward,//התגמול שהתקבל מהפעולה
        const JunctionState& nextState,//מצב הצומת לאחר הפעולה
        const std::vector<Action>& nextValidActions//פעולות חוקיות שניתן לבצע לאחר הפעולה
    );

    // Reward skeleton: waiting-time penalty + emergency priority
    double computeReward(
        const JunctionState& prevState,//מצב הצומת לפני הפעולה
        const JunctionState& nextState,//מצב הצומת לאחר הפעולה
        int actionPhaseId,//הפאזה שנבחרה
        bool emergencyLaneGotGreen,//האם היה אור ירוק בנתיב החירום
        bool greenSyncedWithNeighbor = false,//האם סונכרן עם שכנים
        bool greenOppositeToNeighbor = false,//האם היה מנוגד ויצר הפרעה לשכנים
        double stepSec = 1.0//משך הזמן שלוקח לבצע את הפעולה (ברירת מחדל 1 שנייה)
    ) const;//לא ניתן לשינוי - רק לקריאה

    void decayExploration();//הפחתת האפסילון כדי להפחית את הסיכוי לבחירה אקראית לאורך זמן
    double epsilon() const noexcept;//החזרת ערך האפסילון הנוכחי
    void setThresholdConfig(TrafficThresholdConfig thresholds);//עדכון ספי התנועה
    const TrafficThresholdConfig& thresholdConfig() const noexcept;//קבלת ספי התנועה הנוכחיים
    void setNeighborCoordConfig(NeighborCoordConfig config);//עדכון חוקי התיאום עם שכנים
    const NeighborCoordConfig& neighborCoordConfig() const noexcept;//קבלת חוקי התיאום עם שכנים
    void setRandomSeed(std::uint32_t seed) noexcept;//קביעת זרע אקראי לשחזור התנהגות

    bool loadQTable(const std::string& filePath);// טעינת טבלת הלמידה מקובץ
    bool saveQTable(const std::string& filePath) const;//שמירת טבלת הלמידה לקובץ

private:
    int selectRuleBasedAction(const JunctionState& state, const std::vector<Action>& validActions) const;//בחירה מבוססת על חוקים - אם אנחנו על מצב חמירום או סינכרון עם שכנים, נעדיף את הפעולה הזו על פני למידה
    int discretizeThreeLevel(double value, const ThreeLevelThresholds& thresholds) const;//פונקציה שמחזירה את רמת התנועה (נמוכה, בינונית, גבוהה) בהתבסס על ספי התנועה
    std::string encodeState(const JunctionState& s) const;//פונקציה שמקבלת את מצב הצומת ומחזירה מחרוזת שמייצגת את המצב הזה בצורה ייחודית - כדי שנוכל להשתמש בזה כמפתח בטבלת הלמידה
    std::vector<double>& qValuesFor(const std::string& stateKey, std::size_t actionCount);//מחזירה את ווקטור ערכי הקיו עבור מצב מסוים, ואם אין ערכים קיימים, היא יוצרת ווקטור חדש עם אפסים
    int actionIndexByPhaseId(const std::vector<Action>& validActions, int phaseId) const;//פונקציה שמחזירה את האינדקס של הפאזה שנבחרה בתוך רשימת הפעולות החוקיות
    int phaseIdByActionIndex(const std::vector<Action>& validActions, int actionIndex) const;//פונקציה שמחזירה את מזהה הפאזה עבור אינדקס מסוים ברשימת הפעולות החוקיות

private:
    RLConfig cfg_;//הגדרות למידה
    TrafficThresholdConfig thresholds_;//ספי תנועה
    NeighborCoordConfig neighborCfg_;// חוקי תיאום עם שכנים
    std::unordered_map<std::string, std::vector<double>> qTable_;// טבלת הלמידה - מפתח הוא מחרוזת שמייצגת את מצב הצומת, והערך הוא ווקטור של ערכי קיו עבור כל פעולה חוקית
    mutable std::mt19937 rng_{std::random_device{}()};//גנרטור מספרים אקראיים לשחזור התנהגות
};

} // namespace traffic