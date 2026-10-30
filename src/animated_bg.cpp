#include "animated_bg.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace {
struct Particle {
    ImVec2 pos;
    ImVec2 vel;
    float  size = 1.f;
    ImU32  color = 0;
};

std::vector<Particle> g_particles;
std::mt19937 g_rng{1337u};
std::uniform_real_distribution<float> g_unif{0.f, 1.f};

bool   g_enabled = true;
int    g_count = 160;
float  g_speed = 80.f;
float  g_sizeMin = 1.2f;
float  g_sizeMax = 4.2f;
ImVec4 g_color = ImVec4(0.36f, 0.69f, 1.00f, 0.90f);

ImVec2 randPos(float w, float h) {
    return ImVec2(g_unif(g_rng) * w, g_unif(g_rng) * h);
}

ImVec2 randVel(float speedScale) {
    const float a = g_unif(g_rng) * 2.f * 3.14159265f;
    const float base = 0.25f + g_unif(g_rng) * 0.9f;
    return ImVec2(std::cos(a) * base * speedScale,
                 std::sin(a) * base * speedScale);
}

void ensureParticles(float w, float h) {
    if ((int)g_particles.size() >= g_count) return;
    while ((int)g_particles.size() < g_count) {
        Particle p;
        p.pos = randPos(w, h);
        p.vel = randVel(0.6f + g_unif(g_rng) * 0.9f);
        p.size = g_sizeMin + g_unif(g_rng) * (g_sizeMax - g_sizeMin);
        ImVec4 c = g_color;
        float tint = 0.75f + g_unif(g_rng) * 0.35f;
        c.x *= tint; c.y *= tint; c.z *= tint;
        c.w *= 0.45f + g_unif(g_rng) * 0.6f;
        p.color = ImGui::GetColorU32(c);
        g_particles.push_back(p);
    }
}

} // namespace

void AnimatedBg::tick(float dt) {
    if (!g_enabled) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.f || display.y <= 0.f) return;

    ensureParticles(display.x, display.y);

    for (auto& p : g_particles) {
        p.pos.x += p.vel.x * g_speed * dt;
        p.pos.y += p.vel.y * g_speed * dt;

        if (p.pos.x < -16.f) p.pos.x = display.x + 16.f;
        if (p.pos.x > display.x + 16.f) p.pos.x = -16.f;
        if (p.pos.y < -16.f) p.pos.y = display.y + 16.f;
        if (p.pos.y > display.y + 16.f) p.pos.y = -16.f;

        p.vel.x += (g_unif(g_rng) - 0.5f) * 0.12f;
        p.vel.y += (g_unif(g_rng) - 0.5f) * 0.12f;
        p.vel.x = std::clamp(p.vel.x, -2.5f, 2.5f);
        p.vel.y = std::clamp(p.vel.y, -2.5f, 2.5f);
    }

    if ((int)g_particles.size() > g_count) {
        g_particles.resize(g_count);
    }
}

void AnimatedBg::render() {
    if (!g_enabled) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.f || display.y <= 0.f) return;

    auto* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

    const float maxDist = 110.f;
    for (size_t i = 0; i < g_particles.size(); ++i) {
        const auto& a = g_particles[i];
        drawList->AddCircleFilled(ImVec2(a.pos.x, a.pos.y), a.size * 1.9f,
            ImGui::GetColorU32(ImVec4(g_color.x, g_color.y, g_color.z, 0.10f)), 12);
        drawList->AddCircleFilled(ImVec2(a.pos.x, a.pos.y), a.size,
            a.color, 10);

        for (size_t j = i + 1; j < g_particles.size() && j < i + 8; ++j) {
            const auto& b = g_particles[j];
            const float dx = a.pos.x - b.pos.x;
            const float dy = a.pos.y - b.pos.y;
            const float dist2 = dx * dx + dy * dy;
            if (dist2 > maxDist * maxDist) continue;

            const float dist = std::sqrt(dist2);
            const float alpha = 0.10f * (1.f - dist / maxDist);
            drawList->AddLine(ImVec2(a.pos.x, a.pos.y),
                              ImVec2(b.pos.x, b.pos.y),
                              ImGui::GetColorU32(ImVec4(g_color.x, g_color.y, g_color.z, alpha)),
                              1.0f);
        }
    }
}

void AnimatedBg::showSettings() {
    ImGui::Checkbox("animated background", &g_enabled);
    ImGui::SliderInt("particles", &g_count, 0, 300);
    ImGui::SliderFloat("speed", &g_speed, 10.f, 220.f);
    ImGui::SliderFloat("min size", &g_sizeMin, 0.6f, 6.f);
    ImGui::SliderFloat("max size", &g_sizeMax, 0.6f, 10.f);
    if (g_sizeMin > g_sizeMax) g_sizeMin = g_sizeMax;
    if (g_sizeMax < g_sizeMin) g_sizeMax = g_sizeMin;
    ImGui::ColorEdit4("particle color", &g_color.x, ImGuiColorEditFlags_NoInputs);
}
