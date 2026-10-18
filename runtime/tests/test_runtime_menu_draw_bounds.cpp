#include "runtime_menu_draw_bounds.h"
#include <cassert>
#include <cstdio>

static void custom_callback(const ImDrawList *, const ImDrawCmd *) {}

static bool bounds(float &bottom) {
    float left=3840.f,top=2058.f,right=0.f;
    bottom=0.f;
    const bool full=runtime_menu_draw_bounds(ImGui::GetDrawData(),left,top,right,bottom);
    assert(full || (left<=0.f && top<=0.f && right>=3840.f));
    return full;
}

static void menu_frame(int extra) {
    ImGui::NewFrame();
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextUnformatted("File   Video   Audio");
        ImGui::EndMainMenuBar();
    }
    ImDrawList *fg=ImGui::GetForegroundDrawList();
    if (extra==2) fg->PushClipRect(ImVec2(150,150),ImVec2(200,200),true);
    if (extra) fg->AddRectFilled(ImVec2(120,100),ImVec2(300,220),IM_COL32_WHITE);
    if (extra==2) fg->PopClipRect();
    if (extra==3) fg->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
    if (extra==4) fg->AddCallback(custom_callback,nullptr);
    ImGui::Render();
}

int main() {
    ImGui::CreateContext();
    ImGuiIO &io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.DisplaySize=ImVec2(2560,1372);io.DisplayFramebufferScale=ImVec2(1.5f,1.5f);io.DeltaTime=1.f/60.f;
    unsigned char *pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    float bottom;
    menu_frame(0);assert(!bounds(bottom));assert(bottom>10.f && bottom<80.f);
    std::vector<unsigned char> first,next;
    assert(runtime_menu_draw_signature(ImGui::GetDrawData(),io.Fonts->TexID,3840,2058,first));
    menu_frame(0);
    assert(runtime_menu_draw_signature(ImGui::GetDrawData(),io.Fonts->TexID,3840,2058,next));
    assert(first==next);
    assert(runtime_menu_draw_signature(ImGui::GetDrawData(),io.Fonts->TexID,3841,2058,next));
    assert(first!=next);
    ImDrawVert &vertex=ImGui::GetDrawData()->CmdLists[0]->VtxBuffer[0];
    vertex.col^=1u;
    assert(runtime_menu_draw_signature(ImGui::GetDrawData(),io.Fonts->TexID,3840,2058,next));
    assert(first!=next);vertex.col^=1u;
    // A popup's real vertices expand the union even with a loose full-screen clip.
    menu_frame(1);assert(!bounds(bottom));assert(bottom>=330.f && bottom<=332.f);
    assert(!runtime_menu_draw_signature(ImGui::GetDrawData(),io.Fonts->TexID,3840,2058,next));
    menu_frame(2);assert(!bounds(bottom));assert(bottom>=300.f && bottom<=302.f);
    menu_frame(3);assert(!bounds(bottom));assert(bottom>=330.f && bottom<=332.f);
    menu_frame(4);assert(bounds(bottom));
    menu_frame(0);
    ImDrawList *list=ImGui::GetDrawData()->CmdLists[0];
    const ImDrawIdx saved=list->IdxBuffer[0];list->IdxBuffer[0]=(ImDrawIdx)65535;
    assert(bounds(bottom));list->IdxBuffer[0]=saved;
    ImGui::DestroyContext();
    std::puts("Menu geometry/cache: bar, popup, clipping, DPI, colour changes, callbacks and invalid-index fallback passed");
}
