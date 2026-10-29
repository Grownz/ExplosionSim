#pragma once

// Low-poly vehicle models from the Kenney Car Kit (CC0 1.0,
// https://kenney.nl/assets/car-kit). The .esv files are converted from the
// OBJ sources by scripts/convert-kenney-vehicles.py; each entry stores the
// natural bounding-box size so the scene and renderer agree on the scale.
namespace ex::scene {

struct VehicleModelInfo {
    const wchar_t* asset;
    float width;
    float height;
    float length;
};

inline constexpr VehicleModelInfo kVehicleModels[] = {
    {L"sedan.esv", 1.50f, 1.30f, 2.55f},
    {L"sedan-sports.esv", 1.30f, 1.10f, 2.55f},
    {L"hatchback-sports.esv", 1.30f, 1.10f, 2.85f},
    {L"suv.esv", 1.50f, 1.30f, 2.70f},
    {L"suv-luxury.esv", 1.50f, 1.30f, 2.85f},
    {L"van.esv", 1.50f, 1.35f, 2.75f},
    {L"taxi.esv", 1.50f, 1.50f, 2.75f},
    {L"police.esv", 1.50f, 1.30f, 3.10f},
    {L"truck.esv", 1.50f, 1.30f, 2.95f},
    {L"truck-flat.esv", 1.50f, 1.30f, 2.75f},
};

inline constexpr int kVehicleModelCount =
    int(sizeof(kVehicleModels) / sizeof(kVehicleModels[0]));

} // namespace ex::scene
