#pragma once
#include <cstddef>  // std::size_t

// TrafficConstants.h
// כל קבועי המערכת במקום אחד.
// כל הערכים כאן הם constexpr — לא ניתנים לשינוי בזמן ריצה.

namespace traffic {
namespace constants {

//  RL Agent — כלל הרעב (Starvation Rule) 
// בונוס שניתן לפאזה שמשרתת נתיב עם המתנה ארוכה מאוד.
constexpr double kStrongStarvationBoost  = 50.0;

//  RL Agent — חישוב תגמול (Reward Shaping) 
// חירום: בונוס מיידי כשהנתיב מקבל ירוק, קנס לכל שנייה שמתעכבים.
constexpr double kEmergencyImmediateBonus  = 100.0;
constexpr double kEmergencyDelayPerSecPenalty  =  10.0;

// המתנה: קנס לכל רכב × שניית המתנה.
constexpr double kWaitPerVehiclePerSecPenalty   =   1.0;

// קנסות רעב (נתיב ממתין זמן רב מדי).
constexpr double kHunger60Penalty  =  30.0;  // המתנה > lowMax
constexpr double kHunger90Penalty  =  80.0;  // המתנה > mediumMax

// בונוס על קיצור תור, קנס על הגדלת תור
constexpr double kTotalQueueReductionBonus  =   4.0;
constexpr double kQueueIncreasePenalty  =   3.0;

// קנסות תור מקסימלי.
constexpr double kMaxQueuePenalty  =   0.8;
constexpr double kMaxQueueGrowthPenalty  =   4.0;

//  בקר RL — לולאת שרת (main.cpp) 
// גודל צעד זמן בשניות בין כל איטרציה של הבקר.
constexpr double kControllerStepSec  =   0.5;

//  סימולציה (Simulation.cpp) 
// מספר צעדים להרצה מלאה (verbose).
constexpr int    kSimVerboseSteps = 300;

// מספר צעדים לבדיקת רגרסיה (מהיר יותר).
constexpr int    kSimRegressionSteps = 150;

//  בדיקות (SelfTests.cpp) 
// שיפור מינימלי שנדרש מתיאום שכנים לעומת ללא תיאום.
constexpr double kMinNeighborImprovement         =   0.3;

//  RLConfig — ערכי ברירת מחדל לסוכן הלמידה 
// קצב הלמידה: כמה מהר הסוכן מעדכן את ידיעותיו מכל ניסיון.
constexpr double kDefaultAlpha =  0.10;

// פקטור היוון: מידת ההתחשבות בתגמולים עתידיים (קרוב ל-1 = חשיבה ארוכת טווח).
constexpr double kDefaultGamma =  0.95;

// אפסילון: הסתברות לבחירה אקראית (חקירה) בתחילת האימון.
constexpr double kDefaultEpsilon = 0.15;

// אפסילון מינימלי: ה"רצפה" שמתחתיה האפסילון לא ירד — הסוכן תמיד יחקור קצת.
constexpr double kDefaultEpsilonMin =  0.02;

// קצב דעיכת האפסילון: מכפיל שמוחל בכל איטרציה להפחתת ההתנסות לאורך זמן.
constexpr double kDefaultEpsilonDecay =  0.995;

//  RL Agent — תיאום גל ירוק עם שכנים 
// תגמול לסנכרון פאזה עם שכן: 0.3 כשהתור ירד, 0.1 כשהתור עלה.
constexpr double kGreenSyncPositiveReward        =  0.3;
constexpr double kGreenSyncNegativeReward        =  0.1;

// קנס לניגוד לשכן: 0.6 כשהתור ירד (הפסד קואורדינציה), 0.2 כשהתור עלה.
constexpr double kGreenOppNegativePenalty        =  0.6;
constexpr double kGreenOppPositivePenalty        =  0.2;

//  שרת — טיימינג דביקות פעולה (TrafficServer.cpp) 
// כמה שניות פעולת בקר RL נשארת "דביקה" לפני שניתן לשנותה.
constexpr double kCppActionStickySec             =  5.0;

// כמה שניות פעולה ידנית (מ-UI) נשארת דביקה — ארוכה יותר למניעת החלפות מהירות.
constexpr double kManualActionStickySec          = 20.0;

// כמה שניות אות חירום נשאר פעיל לאחר קבלתו.
constexpr double kEmergencyLatchSec              =  8.0;

//  ניקוד Greedy fallback (TrafficServer.cpp) 
// משקלי ניקוד נתיב: score = vc×W1 + density×W2 + wait×W3.
constexpr double kGreedyWeightVehicleCount       =  1.5;
constexpr double kGreedyWeightDensityPct         =  0.5;
constexpr double kGreedyWeightWaitingSec         =  0.2;

//  מדד ביצועים סימולציה (Simulation.cpp) 
// משקל המתנה ממוצעת ומשקל תפוקה בפונקציית הניקוד (נמוך = טוב יותר).
constexpr double kProfileScoreWaitWeight         = 100.0;
constexpr double kProfileScoreThroughputWeight   =  0.02;

//  שכנים מדומים בסימולציה (Simulation.cpp) 
// מחזורי שלב (בצעדים) לשכן upstream ולשכן downstream.
constexpr int    kNeighborPhaseCycleUpstream     = 20;
constexpr int    kNeighborPhaseCycleDownstream   = 25;

// חלון חירום של שכן downstream בסימולציה.
constexpr int    kNeighborEmergencyWindowStart   = 180;
constexpr int    kNeighborEmergencyWindowEnd     = 190;

// חלון חירום ראשי בסימולציה (בדיקת רגרסיה).
constexpr int    kSimEmergencyWindowStart        = 120;
constexpr int    kSimEmergencyWindowEnd          = 130;

// המרת תור לצפיפות (0–100): density = clamp(queue × factor, 0, 100).
constexpr double kDensityScaleFactor             =  8.0;

// רכבים שעוזבים לשנייה כשהפאזה ירוקה.
constexpr int    kSaturationPerGreenSec          =  2;

//  שרת — JWT ואימות (JwtAuth.cpp / TrafficServer.cpp) 
// משך תוקף טוקן JWT של אדמין (בדקות): 480 = 8 שעות.
constexpr int         kAdminJwtExpirationMinutes =  480;

// אורך מינימלי לשם משתמש.
constexpr std::size_t kMinUsernameLength         =  3;

// אורך מינימלי לסיסמה.
constexpr std::size_t kMinPasswordLength         =  8;

// מספר מינימלי של אדמינים — למניעת מחיקת האדמין האחרון.
constexpr int         kMinAdminCount             =  1;

//  צומת (Junction.h) — ערכי ברירת מחדל לטיימינג פאזות 
// זמן ירוק מינימלי לפאזה — מונע החלפות מהירות מדי כשהנתיב ריק.
constexpr double kDefaultMinGreenSec             =  5.0;

// זמן ירוק מקסימלי לפאזה — מונע הרעבה של נתיבים אחרים.
constexpr double kDefaultMaxGreenSec             = 60.0;

// זמן צהוב בין פאזה לפאזה — מאפשר לרכבים לפנות את הצומת.
constexpr double kDefaultYellowSec               =  2.0;

// זמן "הכל אדום" בין פאזה לפאזה — מרחק בטחון נוסף לפני הפאזה הבאה.
constexpr double kDefaultAllRedSec               =  1.0;

// זמן כולל inter-green (צהוב + הכל אדום) = yellowSec + allRedSec.
constexpr double kDefaultInterGreenSec           =  3.0;

// זמן נסיעה משוער מהשכן לצומת הנוכחית (שניות) — לחיזוי הגעת רכבים.
constexpr int    kDefaultNeighborTravelTimeSec   = 30;

//  אימות הודעות שכנים (main.cpp / NeighborAuthConfig) 
// הפרש מקסימלי מותר בין חותמות זמן להודעות מהשכן (שניות).
constexpr double kDefaultNeighborSignatureSkewSec =  10.0;

//  אימות חירום (TrafficServer.cpp / EmergencyAuthConfig) 
// הפרש מקסימלי מותר בין חותמות זמן להודעות חירום (שניות).
constexpr double kDefaultEmergencyClockSkewSec   = 30.0;

} // namespace constants
} // namespace traffic
