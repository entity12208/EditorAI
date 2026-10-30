@@
 #include <imgui-cocos.hpp>
-#include <algorithm>
+#include <algorithm>
 #include <array>
@@
 #include <imgui-cocos.hpp>
-#include "animated_bg.hpp"
+#include "animated_bg.hpp"
 #include <algorithm>
@@
-void sameLineWrap(float w) {
-    float x    = ImGui::GetCursorPosX();
-    float maxX = ImGui::GetContentRegionMax().x;
-    float sp   = ImGui::GetStyle().ItemSpacing.x;
-    if (!uiMobile() || x + sp + w <= maxX) ImGui::SameLine();
-}
+// SameLine unless the next item of width `w` would run off the right edge;
+// then start a new row (touch-sized buttons don't all fit on one line).
+void sameLineWrap(float w) {
+    if (!uiMobile()) { ImGui::SameLine(); return; }
+    // Compare the right edge of the last item with the pane right edge.
+    float itemMaxX = ImGui::GetItemRectMax().x;
+    float maxX = ImGui::GetContentRegionMax().x;
+    float sp   = ImGui::GetStyle().ItemSpacing.x;
+    if (itemMaxX + sp + w <= maxX) ImGui::SameLine();
+}
@@
-void drawOverlay() {
-    float dt = ImGui::GetIO().DeltaTime;
-    if (!g_themeLoaded) loadTheme();
-    if (!g_st.persistedLoaded) loadPersistedInputs();
-    applyThemeFrame();
-    applyMobileMetrics();
+void drawOverlay() {
+    float dt = ImGui::GetIO().DeltaTime;
+    AnimatedBg::tick(dt);
+    if (!g_themeLoaded) loadTheme();
+    if (!g_st.persistedLoaded) loadPersistedInputs();
+    applyThemeFrame();
+    applyMobileMetrics();
@@
-    if (g_animT <= 0.05f && !g_st.panelOpen) {
+    if (g_animT <= 0.05f && !g_st.panelOpen) {
         // Fully hidden — drop any mid-edit flags so Settings fields re-sync
         // from live values when it reopens. A hotkey capture left dangling
         // would silently eat the next keypress, so it dies here too.
         for (auto& [id, tb] : textBufs()) tb.editing = false;
         g_keyCapture = false;
         return;
     }
     float alpha = g_animT * g_animT * (3.f - 2.f * g_animT);  // smoothstep
+    // Draw background behind the UI while the overlay is visible/appearing
+    AnimatedBg::render();
@@
     if (ImGui::BeginTabBar("##tabs")) {
@@
     if (ImGui::CollapsingHeader("Theme")) {
         ImGui::TextColored(COL_DIM, "Changes apply live.");
+        // Animated background controls
+        AnimatedBg::showSettings();
         for (auto& s : THEME_SLOTS) {
@@
     // Send/Cancel input area
-    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - dp(uiMobile() ? 80.f : 76.f));
-    bool enter = ImGui::InputTextWithHint("##chat", "message the AI...",
-        g_st.chatInput, sizeof(g_st.chatInput),
-        ImGuiInputTextFlags_EnterReturnsTrue | TEXT_SELECTION_FLAGS,
-        textSelectionCallback);
-    ImGui::SameLine();
-    if (sel && sel->state == GenSession::State::Running) {
-        if (ImGui::Button("Cancel", ImVec2(dpw(68.f), 0)))
-            editoraiCancelSession(sel);
-        tipIfHovered("Stop this generation now.");
-    } else {
-        bool can = sel && g_st.chatInput[0] != '\0';
-        ImGui::BeginDisabled(!can);
-        if ((ImGui::Button("Send", ImVec2(dpw(68.f), 0)) || enter) && can) {
-            editoraiSendFollowUp(sel, g_st.chatInput, g_st.chatMode);
-            memset(g_st.chatInput, 0, sizeof(g_st.chatInput));
-        }
-        ImGui::EndDisabled();
-        if (sel && !sel->enginePtr)
-            tipIfHovered("Restored session - sending rebuilds the AI context "
-                         "from the saved conversation. Edit messages may ask "
-                         "you to open the session's level first.");
-        else
-            tipIfHovered("Send a follow-up. The current mode (left) decides "
-                         "whether it edits the level, plans, or just chats.");
-    }
+    // Reserve space for the action button before drawing the input.
+    float actionW = dpw(68.f);                          // desired button width
+    float spacing = ImGui::GetStyle().ItemSpacing.x;
+    float avail = ImGui::GetContentRegionAvail().x;
+    float inputW = std::max(40.f, avail - actionW - spacing);
+    ImGui::SetNextItemWidth(inputW);
+
+    bool enter = ImGui::InputTextWithHint("##chat", "message the AI...",
+        g_st.chatInput, sizeof(g_st.chatInput),
+        ImGuiInputTextFlags_EnterReturnsTrue | TEXT_SELECTION_FLAGS,
+        textSelectionCallback);
+
+    // draw the action button at the reserved width
+    ImGui::SameLine();
+    if (sel && sel->state == GenSession::State::Running) {
+        if (ImGui::Button("Cancel", ImVec2(actionW, 0)))
+            editoraiCancelSession(sel);
+        tipIfHovered("Stop this generation now.");
+    } else {
+        bool can = sel && g_st.chatInput[0] != '\0';
+        ImGui::BeginDisabled(!can);
+        if ((ImGui::Button("Send", ImVec2(actionW, 0)) || enter) && can) {
+            editoraiSendFollowUp(sel, g_st.chatInput, g_st.chatMode);
+            memset(g_st.chatInput, 0, sizeof(g_st.chatInput));
+        }
+        ImGui::EndDisabled();
+        if (sel && !sel->enginePtr)
+            tipIfHovered("Restored session - sending rebuilds the AI context from the saved conversation. Edit messages may ask you to open the session's level first.");
+        else
+            tipIfHovered("Send a follow-up. The current mode (left) decides whether it edits the level, plans, or just chats.");
+    }
