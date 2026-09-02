#pragma once  // מונע הכללה כפולה

#include "../RLAgent.h"  // RLConfig — פרמטרי אלגוריתם ה-RL (אלפא, גמא, נסיון)

#include <string>  // std::string

namespace traffic {  // כל מבני הנתונים של הסרבר נמצאים תחת namespace זה

// פרמטרי טיימינג של הצומת — כמה שניות כל פאזה תהיה ירוקה/צהובה/אדומה
struct RuntimeJunctionConfig {
    double minGreenSec = 5.0;   // זמן ירוק מינימלי לפאזה (שניות)
    double maxGreenSec = 60.0;  // זמן ירוק מקסימלי (שניות)
    double yellowSec   = 2.0;   // זמן צהוב (מעבר בין פאזות)
    double allRedSec   = 1.0;   // זמן אדום כללי (בטיחות לפני מעבר פאזה)
};

// מבנה המאגד את כל פרמטרי הקונפיגורציה הנטענים מקובץ JSON
struct RuntimeConfig {
    RLConfig              rl;       // פרמטרי אלגוריתם RL (אלפא, גמא, נסיון, דעיכה)
    RuntimeJunctionConfig junction; // פרמטרי טיימינג של הצומת
    std::string           source = "defaults";  // מאיפה נטענה הקונפיגורציה ("defaults" / שם הקובץ)
};

// טוען קובץ config.production.json ומחזיר RuntimeConfig מאוכלס
// אם הקובץ חסר או פגום — מחזיר ערכי ברירת מחדל
RuntimeConfig loadRuntimeConfig(const std::string& preferredPath = "");

} // namespace traffic