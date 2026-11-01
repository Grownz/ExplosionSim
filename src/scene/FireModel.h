#pragma once

#include "scene/SceneTypes.h"

namespace ex::scene::fire {

// Fire spread model parameters (docs/physics.md, "Fire spread"). Every value is
// a published engineering constant or an effective value derived from one; the
// source is noted next to each entry in FireModel.cpp.
struct Material {
    // Building fuel load per floor area (kg of wood equivalent per m^2 and
    // floor); zero for objects with a fixed fuel mass.
    float fuelKgPerFloorAreaM2;
    float fuelKg;
    // Effective mass-loss flux over the envelope area (kg / m^2 / s).
    float massFluxKgM2s;
    // Time to reach the fully developed burn rate (t^2 growth), seconds.
    float growthS;
    // Fraction of the facing surface that radiates at the flame emissive power.
    float coverage;
    // Critical heat flux below which the surface does not ignite (W/m^2).
    float criticalFluxWm2;
    // Thermally thin fuel (foliage) accumulates (q - q_cr) * t up to
    // `ignitionParam` [J/m^2]; thermally thick fuel accumulates (q - q_cr)^2 * t
    // up to `ignitionParam` [W^2 s / m^4].
    bool thin;
    double ignitionParam;
};

const Material& MaterialFor(SceneObjectType type);

// Fuel mass of an object at the start of the fire (kg).
float InitialFuelKg(const SceneObject& object);
// Envelope area used for the burn rate (m^2).
float EnvelopeAreaM2(const SceneObject& object);
// Emissive power of the flames (W/m^2) before coverage and growth are applied.
double FlameEmissivePowerWm2();
// Thermal fluence (J/m^2) of a pulse of `pulseS` seconds that ignites the
// material, from the same ignition model as the radiative spread.
float BlastIgnitionFluenceJm2(SceneObjectType type, double pulseS);
// Configuration factor from a rectangle of width `width` and height `height`
// to a small element at distance `distance` on the rectangle's normal through
// its centre, facing it.
double RectangleViewFactor(double width, double height, double distance);

} // namespace ex::scene::fire
