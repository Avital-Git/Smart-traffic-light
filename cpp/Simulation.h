#pragma once

#include <string>

namespace traffic_sim {

struct SimulationResult {
    double bestNeighborScore = 0.0;// הציון הטוב ביותר שנמצא עבור פרופיל תיאום השכנים
    double noNeighborScore = 0.0;// הציון שנמצא כאשר אין תיאום עם השכנים
    double scoreDelta = 0.0;// ההפרש בין הציון של פרופיל התיאום הטוב ביותר לבין הציון של מצב ללא תיאום עם השכנים
    bool neighborImprovement = false;// מציין אם פרופיל התיאום הטוב ביותר שיפר את הציון לעומת מצב ללא תיאום עם השכנים
    std::string bestProfileName;// שם פרופיל התיאום הטוב ביותר שנמצא
};

SimulationResult run_simulation_comparison();// פונקציה שמריצה את הסימולציה ומשווה בין פרופילי התיאום השונים
void run_simulation_comparison_verbose();// פונקציה שמריצה את הסימולציה בצורה מפורטת ומדפיסה את התוצאות

} // namespace traffic_sim
