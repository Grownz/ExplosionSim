#pragma once

// Low-poly vehicle models from the Kenney Car Kit (CC0 1.0,
// https://kenney.nl/assets/car-kit). The .esv files are converted from the
// OBJ sources by scripts/convert-kenney-vehicles.py; each entry stores the
// natural bounding-box size so the scene and renderer agree on the scale.
//
// The physical columns describe the real vehicle class the model stands for,
// not the stylised proxy geometry: average curb mass, drag coefficient and the
// reference areas used by the blast loading and the 4C flow drag. Sources are
// listed in docs/physics.md ("Vehicles"); the values follow the US EPA 2024
// body-style weights and the CARB drag-coefficient survey medians.
namespace ex::scene {

struct VehicleModelInfo {
    const char* asset;
    float width;
    float height;
    float length;
    // Average road-vehicle data for the model's class.
    float massKg;
    float dragCoefficient;
    float frontalAreaM2;
    float sideAreaM2;
    float topAreaM2;
};

inline constexpr VehicleModelInfo kVehicleModels[] = {
    //      asset                  w     h     l      mass    Cd    front  side  top
    {"sedan.esv",              1.50f, 1.30f, 2.55f,  1500.0f, 0.30f, 2.2f, 6.6f, 7.9f},
    {"sedan-sports.esv",       1.30f, 1.10f, 2.55f,  1550.0f, 0.32f, 2.1f, 6.2f, 7.5f},
    {"hatchback-sports.esv",   1.30f, 1.10f, 2.85f,  1250.0f, 0.33f, 2.0f, 5.6f, 6.9f},
    {"suv.esv",                1.50f, 1.30f, 2.70f,  2000.0f, 0.37f, 2.7f, 7.2f, 9.4f},
    {"suv-luxury.esv",         1.50f, 1.30f, 2.85f,  2400.0f, 0.36f, 2.9f, 7.6f, 10.0f},
    {"van.esv",                1.50f, 1.35f, 2.75f,  2100.0f, 0.40f, 3.2f, 7.6f, 9.6f},
    {"taxi.esv",               1.50f, 1.50f, 2.75f,  1500.0f, 0.31f, 2.3f, 6.6f, 8.0f},
    {"police.esv",             1.50f, 1.30f, 3.10f,  1800.0f, 0.31f, 2.4f, 6.8f, 8.5f},
    // The Kenney "truck" reads as a light pickup/box truck, so use the pickup
    // class numbers; the flatbed carries less bodywork and has a higher Cd.
    {"truck.esv",              1.50f, 1.30f, 2.95f,  2500.0f, 0.42f, 3.0f, 7.8f, 9.8f},
    {"truck-flat.esv",         1.50f, 1.30f, 2.75f,  2300.0f, 0.45f, 2.9f, 7.4f, 9.2f},
};

inline constexpr int kVehicleModelCount =
    int(sizeof(kVehicleModels) / sizeof(kVehicleModels[0]));

} // namespace ex::scene
