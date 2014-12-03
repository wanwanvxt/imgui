#define GLEW_STATIC
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#define STB_IMAGE_IMPLEMENTATION
#include "../shared/stb_image.h"                  // for .png loading
#include "../../imgui.h"
#ifdef _MSC_VER
#pragma warning (disable: 4996)         // 'This function or variable may be unsafe': strcpy, strdup, sprintf, vsnprintf, sscanf, fopen
#endif

static GLFWwindow* window;
static GLuint fontTex;
//Shader variables
static int shader_handle, vert_handle, frag_handle;

static int texture_location, ortho_location;
static int position_location, uv_location, colour_location;
//streaming vbo
//we are going to use the same vertex buffer for all rendering of imgui
//so we need to use a large enough buffer as default.
//the buffer will be resized if needed in the rendering code, but it is not a "free" operation.
static size_t vbo_max_size = 1000000; 
static unsigned int vbo_handle, vao_handle;


template<typename T>
unsigned int stream(GLenum target, unsigned int vbo, unsigned int *vbo_cursor, unsigned int *vbo_size, T *start, int elementCount)
{
    unsigned int bytes = sizeof(T) *elementCount;
    unsigned int aligned = bytes + bytes % 64; //align memory

    glBindBuffer(target, vbo);
    //If there's not enough space left, orphan the buffer object, create a new one and start writing
    if (vbo_cursor[0] + aligned > vbo_size[0])
    {
        assert(aligned < vbo_size[0]);
        glBufferData(target, vbo_size[0], NULL, GL_DYNAMIC_DRAW);
        vbo_cursor[0] = 0;
    }

    void* mapped = glMapBufferRange(target, vbo_cursor[0], aligned, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

    memcpy(mapped, start, bytes);
    vbo_cursor[0] += aligned;

    glUnmapBuffer(target);
    return vbo_cursor[0] - aligned; //return the offset we use for glVertexAttribPointer call
}

// This is the main rendering function that you have to implement and provide to ImGui (via setting up 'RenderDrawListsFn' in the ImGuiIO structure)
// If text or lines are blurry when integrating ImGui in your engine:
// - try adjusting ImGui::GetIO().PixelCenterOffset to 0.0f or 0.5f
// - in your Render function, try translating your projection matrix by (0.5f,0.5f) or (0.375f,0.375f)
static void ImImpl_RenderDrawLists(ImDrawList** const cmd_lists, int cmd_lists_count)
{
    if (cmd_lists_count == 0)
        return;

    // Setup render state: alpha-blending enabled, no face culling, no depth testing, scissor enabled
    glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);

    // Setup texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fontTex);

    // Setup orthographic projection matrix
    const float width = ImGui::GetIO().DisplaySize.x;
    const float height = ImGui::GetIO().DisplaySize.y;
	const float ortho_projection[4][4] =
	{
		{ 2.0f/width,	0.0f,			0.0f,		0.0f },
		{ 0.0f,			2.0f/-height,	0.0f,		0.0f },
		{ 0.0f,			0.0f,			-1.0f,		0.0f },
		{ -1.0f,		1.0f,			0.0f,		1.0f },
	};
    glUseProgram(shader_handle);
    glUniform1i(texture_location, 0);
    glUniformMatrix4fv(ortho_location, 1, GL_FALSE, &ortho_projection[0][0]);

	// we will update the buffer in full, so we need to compute the total size of the buffer.
	size_t total_vtx_count = 0;
	for (int n = 0; n < cmd_lists_count; n++)
		total_vtx_count += cmd_lists[n]->vtx_buffer.size();
	
	glBindBuffer(GL_ARRAY_BUFFER, vbo_handle);
	size_t neededBufferSize = total_vtx_count * sizeof(ImDrawVert);
	if (neededBufferSize > vbo_max_size)
	{
		vbo_max_size = neededBufferSize;
		glBufferData(GL_ARRAY_BUFFER, total_vtx_count * sizeof(ImDrawVert), NULL, GL_STREAM_DRAW);
	}

	unsigned char* buffer_data = (unsigned char*)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
	if (!buffer_data)
		return;
	for (int n = 0; n < cmd_lists_count; n++)
	{
		const ImDrawList* cmd_list = cmd_lists[n];
		memcpy(buffer_data, &cmd_list->vtx_buffer[0], cmd_list->vtx_buffer.size() * sizeof(ImDrawVert));
		buffer_data += cmd_list->vtx_buffer.size() * sizeof(ImDrawVert);
	}
	glUnmapBuffer(GL_ARRAY_BUFFER);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glBindVertexArray(vao_handle);
    
	int cmd_offset = 0;
	for (int n = 0; n < cmd_lists_count; n++)
    {
        const ImDrawList* cmd_list = cmd_lists[n];
        int vtx_offset = cmd_offset;
        const ImDrawCmd* pcmd_end = cmd_list->commands.end();
        for (const ImDrawCmd* pcmd = cmd_list->commands.begin(); pcmd != pcmd_end; pcmd++)
        {
            glScissor((int)pcmd->clip_rect.x, (int)(height - pcmd->clip_rect.w), (int)(pcmd->clip_rect.z - pcmd->clip_rect.x), (int)(pcmd->clip_rect.w - pcmd->clip_rect.y));
            glDrawArrays(GL_TRIANGLES, vtx_offset, pcmd->vtx_count);
            vtx_offset += pcmd->vtx_count;
        }
		cmd_offset = vtx_offset;
    }

	glBindVertexArray(0);
	glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
	glBindTexture(GL_TEXTURE_2D, 0);

}

static const char* ImImpl_GetClipboardTextFn()
{
    return glfwGetClipboardString(window);
}

static void ImImpl_SetClipboardTextFn(const char* text)
{
    glfwSetClipboardString(window, text);
}

// GLFW callbacks to get events
static void glfw_error_callback(int error, const char* description)
{
    fputs(description, stderr);
}

static void glfw_scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    ImGuiIO& io = ImGui::GetIO();
    io.MouseWheel = (yoffset != 0.0f) ? yoffset > 0.0f ? 1.0f : -1.0f : 0.0f;           // Mouse wheel: -1,0,+1
}

static void glfw_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    ImGuiIO& io = ImGui::GetIO();
    if (action == GLFW_PRESS)
        io.KeysDown[key] = true;
    if (action == GLFW_RELEASE)
        io.KeysDown[key] = false;
    io.KeyCtrl = (mods & GLFW_MOD_CONTROL) != 0;
    io.KeyShift = (mods & GLFW_MOD_SHIFT) != 0;
}

static void glfw_char_callback(GLFWwindow* window, unsigned int c)
{
    if (c > 0 && c <= 255)
        ImGui::GetIO().AddInputCharacter((char)c);
}


// OpenGL code based on http://open.gl tutorials
void InitGL()
{
    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit())
        exit(1);

    glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window = glfwCreateWindow(1280, 720, "ImGui OpenGL example", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSetKeyCallback(window, glfw_key_callback);
    glfwSetScrollCallback(window, glfw_scroll_callback);
    glfwSetCharCallback(window, glfw_char_callback);

	glewExperimental = GL_TRUE;
	
	GLenum err = glewInit();
	if (GLEW_OK != err)
	{
		fprintf(stderr, "Error: %s\n", glewGetErrorString(err));
	}

    const GLchar *vertex_shader = \
        "#version 330\n"
        "uniform mat4 ortho;\n"

        "in vec2 Position;\n"
        "in vec2 UV;\n"
        "in vec4 Colour;\n"

        "out vec2 Frag_UV;\n"
        "out vec4 Frag_Colour;\n"

        "void main()\n"
        "{\n"
        "	Frag_UV = UV;\n"
        "	Frag_Colour = Colour;\n"
        "\n"
        "	gl_Position = ortho*vec4(Position.xy,0,1);\n"
        "}\n";

	const GLchar* fragment_shader = \
        "#version 330\n"
        "uniform sampler2D Texture;\n"

        "in vec2 Frag_UV;\n"
        "in vec4 Frag_Colour;\n"

        "out vec4 FragColor;\n"

        "void main()\n"
        "{\n"
        "	FragColor = Frag_Colour * texture( Texture, Frag_UV.st);\n"
        "}\n";

	shader_handle = glCreateProgram();
	vert_handle = glCreateShader(GL_VERTEX_SHADER);
    frag_handle = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vert_handle, 1, &vertex_shader, 0);
    glShaderSource(frag_handle, 1, &fragment_shader, 0);
    glCompileShader(vert_handle);
    glCompileShader(frag_handle);
    glAttachShader(shader_handle, vert_handle);
    glAttachShader(shader_handle, frag_handle);
    glLinkProgram(shader_handle);

    texture_location = glGetUniformLocation(shader_handle, "Texture");
    ortho_location = glGetUniformLocation(shader_handle, "ortho");
    position_location = glGetAttribLocation(shader_handle, "Position");
    uv_location = glGetAttribLocation(shader_handle, "UV");
    colour_location = glGetAttribLocation(shader_handle, "Colour");

    glGenBuffers(1, &vbo_handle);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_handle);
    glBufferData(GL_ARRAY_BUFFER, vbo_max_size, NULL, GL_DYNAMIC_DRAW);

	glGenVertexArrays(1, &vao_handle);
	glBindVertexArray(vao_handle);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_handle);
    glEnableVertexAttribArray(position_location);
    glEnableVertexAttribArray(uv_location);
    glEnableVertexAttribArray(colour_location);

	glVertexAttribPointer(position_location, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (GLvoid*) offsetof(ImDrawVert, pos));
	glVertexAttribPointer(uv_location, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (GLvoid*) offsetof(ImDrawVert, uv));
	glVertexAttribPointer(colour_location, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (GLvoid*) offsetof(ImDrawVert, col));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	

}

void InitImGui()
{
    int w, h;
    glfwGetWindowSize(window, &w, &h);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);        // Display size, in pixels. For clamping windows positions.
    io.DeltaTime = 1.0f / 60.0f;                        // Time elapsed since last frame, in seconds (in this sample app we'll override this every frame because our timestep is variable)
    io.PixelCenterOffset = 0.5f;                        // Align OpenGL texels
    io.KeyMap[ImGuiKey_Tab] = GLFW_KEY_TAB;             // Keyboard mapping. ImGui will use those indices to peek into the io.KeyDown[] array.
    io.KeyMap[ImGuiKey_LeftArrow] = GLFW_KEY_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = GLFW_KEY_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = GLFW_KEY_UP;
    io.KeyMap[ImGuiKey_DownArrow] = GLFW_KEY_DOWN;
    io.KeyMap[ImGuiKey_Home] = GLFW_KEY_HOME;
    io.KeyMap[ImGuiKey_End] = GLFW_KEY_END;
    io.KeyMap[ImGuiKey_Delete] = GLFW_KEY_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = GLFW_KEY_BACKSPACE;
    io.KeyMap[ImGuiKey_Enter] = GLFW_KEY_ENTER;
    io.KeyMap[ImGuiKey_Escape] = GLFW_KEY_ESCAPE;
    io.KeyMap[ImGuiKey_A] = GLFW_KEY_A;
    io.KeyMap[ImGuiKey_C] = GLFW_KEY_C;
    io.KeyMap[ImGuiKey_V] = GLFW_KEY_V;
    io.KeyMap[ImGuiKey_X] = GLFW_KEY_X;
    io.KeyMap[ImGuiKey_Y] = GLFW_KEY_Y;
    io.KeyMap[ImGuiKey_Z] = GLFW_KEY_Z;

    io.RenderDrawListsFn = ImImpl_RenderDrawLists;
    io.SetClipboardTextFn = ImImpl_SetClipboardTextFn;
    io.GetClipboardTextFn = ImImpl_GetClipboardTextFn;

    // Load font texture
    glGenTextures(1, &fontTex);
    glBindTexture(GL_TEXTURE_2D, fontTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const void* png_data;
    unsigned int png_size;
    ImGui::GetDefaultFontData(NULL, NULL, &png_data, &png_size);
    int tex_x, tex_y, tex_comp;
    void* tex_data = stbi_load_from_memory((const unsigned char*)png_data, (int)png_size, &tex_x, &tex_y, &tex_comp, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_x, tex_y, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex_data);
    stbi_image_free(tex_data);
}

void UpdateImGui()
{
    ImGuiIO& io = ImGui::GetIO();

    // Setup timestep
    static double time = 0.0f;
    const double current_time =  glfwGetTime();
    io.DeltaTime = (float)(current_time - time);
    time = current_time;

    // Setup inputs
    // (we already got mouse wheel, keyboard keys & characters from glfw callbacks polled in glfwPollEvents())
    double mouse_x, mouse_y;
    glfwGetCursorPos(window, &mouse_x, &mouse_y);
    io.MousePos = ImVec2((float)mouse_x, (float)mouse_y);                           // Mouse position, in pixels (set to -1,-1 if no mouse / on another screen, etc.)
    io.MouseDown[0] = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) != 0;
    io.MouseDown[1] = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) != 0;

    // Start the frame
    ImGui::NewFrame();
}

// Application code
int main(int argc, char** argv)
{
    InitGL();
    InitImGui();

    while (!glfwWindowShouldClose(window))
    {
        ImGuiIO& io = ImGui::GetIO();
        io.MouseWheel = 0;
        glfwPollEvents();
        UpdateImGui();

        // Create a simple window
        // Tip: if we don't call ImGui::Begin()/ImGui::End() the widgets appears in a window automatically called "Debug"
        static bool show_test_window = true;
        static bool show_another_window = false;
        static float f;
        ImGui::Text("Hello, world!");
        ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
        show_test_window ^= ImGui::Button("Test Window");
        show_another_window ^= ImGui::Button("Another Window");

        // Calculate and show framerate
        static float ms_per_frame[120] = { 0 };
        static int ms_per_frame_idx = 0;
        static float ms_per_frame_accum = 0.0f;
        ms_per_frame_accum -= ms_per_frame[ms_per_frame_idx];
        ms_per_frame[ms_per_frame_idx] = ImGui::GetIO().DeltaTime * 1000.0f;
        ms_per_frame_accum += ms_per_frame[ms_per_frame_idx];
        ms_per_frame_idx = (ms_per_frame_idx + 1) % 120;
        const float ms_per_frame_avg = ms_per_frame_accum / 120;
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", ms_per_frame_avg, 1000.0f / ms_per_frame_avg);

        // Show the ImGui test window
        // Most of user example code is in ImGui::ShowTestWindow()
        if (show_test_window)
        {
            ImGui::SetNewWindowDefaultPos(ImVec2(650, 20));        // Normally user code doesn't need/want to call it because positions are saved in .ini file anyway. Here we just want to make the demo initial state a bit more friendly!
            ImGui::ShowTestWindow(&show_test_window);
        }

        // Show another simple window
        if (show_another_window)
        {
            ImGui::Begin("Another Window", &show_another_window, ImVec2(200,100));
            ImGui::Text("Hello");
            ImGui::End();
        }

        // Rendering
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.8f, 0.6f, 0.6f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui::Render();
        glfwSwapBuffers(window);
    }
	if (vao_handle) glDeleteVertexArrays(1, &vao_handle);
    if (vbo_handle) glDeleteBuffers(1, &vbo_handle);
    glDetachShader(shader_handle, vert_handle);
    glDetachShader(shader_handle, frag_handle);
    glDeleteShader(vert_handle);
    glDeleteShader(frag_handle);
    glDeleteProgram(shader_handle);

    ImGui::Shutdown();
    glfwTerminate();

    return 0;
}