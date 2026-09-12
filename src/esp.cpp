#include "esp.h"
#include "offsets.h"
#include <cstdio>

static ImColor TeamColor(int playerTeam, int localTeam) {
    return (playerTeam == localTeam)
        ? ImColor(50, 220, 50, 255)   // teammate = green
        : ImColor(220, 50, 50, 255);  // enemy = red
}

static void DrawBone(ImDrawList* dl, const Player& p, int b1, int b2, ImColor col) {
    if (p.boneOnScreen[b1] && p.boneOnScreen[b2]) {
        dl->AddLine(
            ImVec2(p.boneScreen[b1].x, p.boneScreen[b1].y),
            ImVec2(p.boneScreen[b2].x, p.boneScreen[b2].y),
            col, 1.5f
        );
    }
}

void DrawESP(const std::vector<Player>& players, int localTeam) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    for (const auto& p : players) {
        if (!p.onScreen) continue;

        ImColor col = TeamColor(p.team, localTeam);
        ImColor white(255, 255, 255, 255);
        ImColor black(0, 0, 0, 200);

        float sx = p.screenFeet.x;
        float sy = p.screenFeet.y;
        float hx = p.screenHead.x;
        float hy = p.screenHead.y;

        float boxH = sy - hy;
        float boxW = boxH * 0.45f;
        float boxX = hx - boxW * 0.5f;
        float boxY = hy;

        // ── Box ESP ──────────────────────────────────────
        // Thin black outline for contrast
        dl->AddRect(
            ImVec2(boxX - 1, boxY - 1),
            ImVec2(boxX + boxW + 1, boxY + boxH + 1),
            black, 0, 0, 3.0f
        );
        dl->AddRect(
            ImVec2(boxX, boxY),
            ImVec2(boxX + boxW, boxY + boxH),
            col, 0, 0, 1.5f
        );

        // ── Health bar ───────────────────────────────────
        float barX    = boxX - 6;
        float barFill = (p.health / 100.0f) * boxH;
        ImColor healthCol(
            (int)(255 * (1.0f - p.health / 100.0f)),
            (int)(255 * (p.health / 100.0f)),
            0, 255
        );
        // background
        dl->AddRectFilled(ImVec2(barX - 1, boxY), ImVec2(barX + 3, boxY + boxH), black);
        // fill (bottom-anchored)
        dl->AddRectFilled(
            ImVec2(barX, boxY + boxH - barFill),
            ImVec2(barX + 2, boxY + boxH),
            healthCol
        );

        // ── Head dot ESP ─────────────────────────────────
        if (p.boneOnScreen[bones::HEAD]) {
            float hbx = p.boneScreen[bones::HEAD].x;
            float hby = p.boneScreen[bones::HEAD].y;
            dl->AddCircleFilled(ImVec2(hbx, hby), 4.0f, col);
            dl->AddCircle(ImVec2(hbx, hby), 4.0f, black, 16, 1.0f);
        }

        // ── Skeleton ESP ─────────────────────────────────
        ImColor skelCol = col;
        // Head → Neck → Spine1 → Spine2 → Pelvis
        DrawBone(dl, p, bones::HEAD,   bones::NECK,   skelCol);
        DrawBone(dl, p, bones::NECK,   bones::SPINE1, skelCol);
        DrawBone(dl, p, bones::SPINE1, bones::SPINE2, skelCol);
        DrawBone(dl, p, bones::SPINE2, bones::PELVIS, skelCol);
        // Left arm
        DrawBone(dl, p, bones::NECK,    bones::ARM_UP_L, skelCol);
        DrawBone(dl, p, bones::ARM_UP_L,bones::ARM_LO_L, skelCol);
        DrawBone(dl, p, bones::ARM_LO_L,bones::HAND_L,   skelCol);
        // Right arm
        DrawBone(dl, p, bones::NECK,    bones::ARM_UP_R, skelCol);
        DrawBone(dl, p, bones::ARM_UP_R,bones::ARM_LO_R, skelCol);
        DrawBone(dl, p, bones::ARM_LO_R,bones::HAND_R,   skelCol);
        // Left leg
        DrawBone(dl, p, bones::PELVIS,  bones::LEG_UP_L, skelCol);
        DrawBone(dl, p, bones::LEG_UP_L,bones::LEG_LO_L, skelCol);
        DrawBone(dl, p, bones::LEG_LO_L,bones::ANKLE_L,  skelCol);
        // Right leg
        DrawBone(dl, p, bones::PELVIS,  bones::LEG_UP_R, skelCol);
        DrawBone(dl, p, bones::LEG_UP_R,bones::LEG_LO_R, skelCol);
        DrawBone(dl, p, bones::LEG_LO_R,bones::ANKLE_R,  skelCol);

        // ── Text ESP: Name / Weapon / Health / Bomb ──────
        char infoName[96];
        snprintf(infoName, sizeof(infoName), "%s", p.name.c_str());

        char infoHp[32];
        snprintf(infoHp, sizeof(infoHp), "HP: %d", p.health);

        char infoWep[64];
        snprintf(infoWep, sizeof(infoWep), "[%s]", p.weapon.c_str());

        // Name above box
        dl->AddText(ImVec2(boxX + boxW * 0.5f - 20, boxY - 14), white,   infoName);
        // HP below name
        dl->AddText(ImVec2(boxX + boxW + 4,           boxY),     healthCol, infoHp);
        // Weapon below box
        dl->AddText(ImVec2(boxX, boxY + boxH + 2),               col,     infoWep);

        // Bomb indicator if player is carrying C4
        if (p.weapon == "C4") {
            dl->AddText(ImVec2(boxX, boxY + boxH + 14),
                ImColor(255, 200, 0, 255), "[BOMB]");
        }
    }
}
