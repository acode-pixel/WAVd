#define SDL_MAIN_USE_CALLBACKS 1
#define IMGUI_DEFINE_MATH_OPERATORS
#include "RtAudio.h"
#include <cstdlib>
#include <fileapi.h>
#include <Windows.h>
#include <iostream>
#include <cstdio>
#include <thread>
#include "SDL3/SDL.h"
#include "SDL3/SDL_main.h"
#include <SDL3/SDL_opengl.h>
#include "imgui.h"
#include "implot.h"
#include "backend/imgui_impl_sdl3.h"
#include "backend/imgui_impl_opengl3.h"
#include "imgui_memory_editor.h"

#define PCM_FORMAT 1

using namespace std;

struct format_chunk {
	unsigned short audio_format;
	unsigned short num_channels;
	unsigned int sample_rate;
	unsigned int byte_rate;
	unsigned short block_align;
	unsigned short bits_per_sample;
	unsigned int bytes_per_sample;
};

struct wave_data {
	unsigned int data_size;
	short int* data_ptr;
};

struct waveform {
	struct format_chunk fmt_chunk;
	struct wave_data wave_data;
};

struct wave_file {
	HANDLE file;
	HANDLE hmap;
	struct waveform file_data;
	LPVOID mapped_data;
};

struct appstate {
	SDL_Window* window;
	SDL_GLContext gl_context;
	ImGuiIO io;
	bool active;
	float my_color[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
	wchar_t current_file[MAX_PATH];
	wchar_t* filename;
	struct wave_file* current_wave;
	bool loadFile = false, playingAudio = false, pauseAudio = false;
	double current_time = 0.0;
	size_t start_addr = 0;
	size_t end_addr = 0;
	size_t current_addr = 0;
	double audio_dt = 0;
	double num_seconds = 0;
	int bufferFrames = 0;
};

RtAudio* audio;

static MemoryEditor mem_edit_2;

static void mainApp(struct appstate* state);

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
	setlocale(LC_ALL, "");
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
		SDL_Log(SDL_GetError());
		return SDL_APP_FAILURE;
	}
	*appstate = malloc(sizeof(struct appstate));
	memset(*appstate, 0, sizeof(struct appstate));
	((struct appstate*)(*appstate))->window = SDL_CreateWindow("WAVd", 800, 600, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	((struct appstate*)(*appstate))->gl_context = SDL_GL_CreateContext(((struct appstate*)(*appstate))->window);
	if(!((struct appstate*)(*appstate))->window || !((struct appstate*)(*appstate))->gl_context) {
		SDL_Log(SDL_GetError());
		return SDL_APP_FAILURE;
	}

	SDL_GL_MakeCurrent(((struct appstate*)(*appstate))->window, ((struct appstate*)(*appstate))->gl_context);
	SDL_GL_SetSwapInterval(1); // Enable vsync
	SDL_SetWindowPosition(((struct appstate*)(*appstate))->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_ShowWindow(((struct appstate*)(*appstate))->window);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();
	((struct appstate*)(*appstate))->io = ImGui::GetIO();
	((struct appstate*)(*appstate))->io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	((struct appstate*)(*appstate))->active = true;
	audio = new RtAudio();

	// Setup Platform/Renderer backends
	ImGui_ImplSDL3_InitForOpenGL(((struct appstate*)(*appstate))->window, ((struct appstate*)(*appstate))->gl_context);
	ImGui_ImplOpenGL3_Init();

	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	struct appstate* state = (struct appstate*)(appstate);
	state->io = ImGui::GetIO();
	if (state->active) {
		mainApp(state);
	}
	ImGui::Render();
	glViewport(0, 0, (int)state->io.DisplaySize.x, (int)state->io.DisplaySize.y);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	SDL_GL_SwapWindow(state->window);
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
	SDL_QuitSubSystem(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImPlot::DestroyContext();
	ImGui::DestroyContext();
	delete (struct appstate*)appstate;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
	if (event->type == SDL_EVENT_QUIT) {
		return SDL_APP_SUCCESS;
	}
	ImGui_ImplSDL3_ProcessEvent(event);
	return SDL_APP_CONTINUE;
}

// Two-channel sawtooth wave generator.
static int saw(void* outputBuffer, void* inputBuffer, unsigned int nBufferFrames,
	double streamTime, RtAudioStreamStatus status, void* userData)
{
	struct appstate* state = (struct appstate*)userData;
	short* buffer = (short*)outputBuffer;
	short* data = (short*)state->current_addr;

	for (unsigned int i = 0; i < nBufferFrames; i++) {
		for (int channel = 0; channel < state->current_wave->file_data.fmt_chunk.num_channels; channel++) {
			if ((int)state->current_time < (int)state->num_seconds) {
				*buffer = *data;
				buffer++;
				data++;
				state->current_addr += state->current_wave->file_data.fmt_chunk.bytes_per_sample;
			}
			else {
				state->playingAudio = false;
				*buffer++ = 0;
			}
		}
	}
	state->audio_dt = nBufferFrames / (double)state->current_wave->file_data.fmt_chunk.sample_rate;
	state->current_time += state->audio_dt;
	return 0;
}

struct wave_file* analyzeWAV(wchar_t* fileSelected)
{
	LPCWSTR wav_filepath = fileSelected;
	HANDLE file = CreateFileW(wav_filepath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		fwprintf(stderr, L"Failed to open file: %ls\n", wav_filepath);
		return NULL;
	}

	HANDLE hmap = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
	if (hmap == NULL) {
		fwprintf(stderr, L"Failed to create file mapping: %ls\n", wav_filepath);
		CloseHandle(file);
		return NULL;
	}

	LPVOID file_data = MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0);
	if (file_data == NULL) {
		fwprintf(stderr, L"Failed to map view of file: %ls\n", wav_filepath);
		CloseHandle(hmap);
		CloseHandle(file);
		return NULL;
	}

	struct wave_file* wav_file = (struct wave_file*)malloc(sizeof(struct wave_file));
	int extra_bytes_to_skip = 0;
	ZeroMemory(wav_file, sizeof(struct wave_file));
	wav_file->file = file;
	wav_file->hmap = hmap;
	wav_file->mapped_data = file_data;

	wav_file->file_data.fmt_chunk.audio_format = *(unsigned short*)((char*)file_data + 20);
	wav_file->file_data.fmt_chunk.num_channels = *(unsigned short*)((char*)file_data + 22);
	wav_file->file_data.fmt_chunk.sample_rate = *(unsigned int*)((char*)file_data + 24);
	wav_file->file_data.fmt_chunk.byte_rate = *(unsigned int*)((char*)file_data + 28);
	wav_file->file_data.fmt_chunk.block_align = *(unsigned short*)((char*)file_data + 32);
	if (wav_file->file_data.fmt_chunk.audio_format == PCM_FORMAT) {
		wav_file->file_data.fmt_chunk.bits_per_sample = *(unsigned short*)((char*)file_data + 34);
		wav_file->file_data.fmt_chunk.bytes_per_sample = wav_file->file_data.fmt_chunk.bits_per_sample / 8;
	}

	if (strncmp((char*)file_data+36, "LIST", 4) == 0) {
		extra_bytes_to_skip = *(unsigned int*)((char*)file_data + 40) + 8;
	}

	wav_file->file_data.wave_data.data_size = *(unsigned int*)((char*)file_data + 36 + extra_bytes_to_skip + 4);
	wav_file->file_data.wave_data.data_ptr = (short int*)((char*)file_data + 36 + extra_bytes_to_skip + 8);
	return wav_file;
	//CloseHandle(file);
	//CloseHandle(hmap);
	//UnmapViewOfFile(file_data);
}

static void load_file(struct appstate* state) {
	// close previous file if open
	if (state->current_wave) {
		UnmapViewOfFile(state->current_wave->mapped_data);
		CloseHandle(state->current_wave->hmap);
		CloseHandle(state->current_wave->file);
		free(state->current_wave);
		state->current_wave = NULL;
		state->current_time = 0;
	}

	// open windows file explorer select file dialog and get path to file
	wchar_t file_path[MAX_PATH];
	OPENFILENAMEW ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	ofn.lpstrFile = file_path;
	ofn.lpstrFile[0] = '\0';
	ofn.nMaxFile = sizeof(file_path);
	ofn.lpstrFilter = L"WAV Files\0*.wav\0All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameW(&ofn)) {
		wprintf(L"Selected file: %ls\n", ofn.lpstrFile);
		state->current_file[0] = '\0';
		wcscpy_s(state->current_file, ofn.lpstrFile);
		state->filename = state->current_file + ofn.nFileOffset;
		state->current_wave = analyzeWAV(state->current_file);
		state->start_addr = (size_t)state->current_wave->file_data.wave_data.data_ptr;
		state->current_addr = state->start_addr;
		state->bufferFrames = state->current_wave->file_data.fmt_chunk.sample_rate;
	}
	else {
		wprintf(L"File selection cancelled.\n");
	}
	state->loadFile = false;
}

static void load_graphs(struct appstate* state) {
	if (ImPlot::BeginSubplots("Waveform", (int)state->current_wave->file_data.fmt_chunk.num_channels, 1, ImVec2(-1, -1), ImPlotSubplotFlags_LinkCols)) {
		for (int channel = 0; channel < state->current_wave->file_data.fmt_chunk.num_channels; channel++) {
			char buf[50];
			snprintf(buf, sizeof(buf), "Channel %d", channel + 1);
			ImPlot::BeginPlot(buf, ImVec2(-1, -1));
			ImPlot::SetupAxes("Time (s)", "Amplitude");
			ImPlot::SetupAxisLimits(ImAxis_Y1, -32768, 32767, ImPlotCond_Always);
			if(state->playingAudio)
				ImPlot::SetupAxisLimits(ImAxis_X1, state->current_time - state->audio_dt, state->current_time, ImPlotCond_Always);

			int num_samples = state->current_wave->file_data.wave_data.data_size / (state->current_wave->file_data.fmt_chunk.bytes_per_sample);
			num_samples = num_samples / state->current_wave->file_data.fmt_chunk.num_channels;
			state->num_seconds = (double)num_samples / state->current_wave->file_data.fmt_chunk.sample_rate;

			ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, state->num_seconds);

			int downsample = (int)(ImPlot::GetPlotLimits().X.Size()) < 1 ? 1 : (int)(ImPlot::GetPlotLimits().X.Size());
			int start = (int)(ImPlot::GetPlotLimits().X.Min * state->current_wave->file_data.fmt_chunk.sample_rate);
			start = start < 0 ? 0 : start > num_samples - 1 ? num_samples - 1 : start;
			int end = (int)(ImPlot::GetPlotLimits().X.Max * state->current_wave->file_data.fmt_chunk.sample_rate);
			end = end < 0 ? 0 : end > num_samples - 1 ? num_samples - 1 : end;
			long int size = (end - start) / downsample;
			short int* audio_data = state->current_wave->file_data.wave_data.data_ptr;

			ImGui::Text("Displaying samples %d to %d (downsampled by %d)", start, end, downsample);

			state->start_addr = (size_t)audio_data + (start * state->current_wave->file_data.fmt_chunk.block_align) + (channel * state->current_wave->file_data.fmt_chunk.bytes_per_sample);

			ImPlot::PlotLine("Audio Data", (short*)state->start_addr, size, 1.0 * downsample / state->current_wave->file_data.fmt_chunk.sample_rate, ImPlot::GetPlotLimits().X.Min, { ImPlotProp_Stride, state->current_wave->file_data.fmt_chunk.block_align * downsample});
			ImPlot::DragLineX(0, &state->current_time, ImVec4(1, 0, 0, 1), 1.0);

			state->current_time = state->current_time < 0 ? 0 : state->current_time > state->num_seconds ? state->num_seconds : state->current_time;

			ImPlot::TagX(state->current_time, ImVec4(1, 0, 0, 1), "%.3f", state->current_time);

			state->end_addr = (size_t)audio_data + (end * state->current_wave->file_data.fmt_chunk.block_align) + (channel * state->current_wave->file_data.fmt_chunk.bytes_per_sample);
			state->current_addr = (size_t)audio_data + (int)(state->current_time * state->current_wave->file_data.fmt_chunk.sample_rate) * state->current_wave->file_data.fmt_chunk.block_align + (channel * state->current_wave->file_data.fmt_chunk.bytes_per_sample);
			state->bufferFrames = (int)((ImPlot::GetPlotLimits().X.Max - ImPlot::GetPlotLimits().X.Min) * state->current_wave->file_data.fmt_chunk.sample_rate);
			state->bufferFrames = state->bufferFrames < state->current_wave->file_data.fmt_chunk.sample_rate ? state->bufferFrames : state->current_wave->file_data.fmt_chunk.sample_rate;
			state->bufferFrames = state->bufferFrames > 0 ? state->bufferFrames : state->current_wave->file_data.fmt_chunk.sample_rate;

			ImPlot::EndPlot();
		}
		ImPlot::EndSubplots();
	}
}

void playAudio(struct appstate* state) {
	RtAudio* dac = audio;
	if (dac->getDeviceCount() < 1) {
		if(ImGui::BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("No audio devices found!");
			if (ImGui::Button("OK", ImVec2(120, 0))) {ImGui::CloseCurrentPopup();}
			ImGui::EndPopup();
		}
		return;
	}
	RtAudio::StreamParameters parameters;
	parameters.deviceId = dac->getDefaultOutputDevice();
	parameters.nChannels = state->current_wave->file_data.fmt_chunk.num_channels;
	parameters.firstChannel = 0;
	double sampleRate = state->current_wave->file_data.fmt_chunk.sample_rate;
	unsigned int bufferFrames = state->bufferFrames;

	if (dac->openStream(&parameters, NULL, state->current_wave->file_data.fmt_chunk.bytes_per_sample, sampleRate, &bufferFrames, &saw, state)) {
		if(ImGui::BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("Failed to open audio stream! Error: %s", dac->getErrorText().c_str());
			if (ImGui::Button("OK", ImVec2(120, 0))) {ImGui::CloseCurrentPopup();}
			ImGui::EndPopup();
		}
		return;
	}

	if (dac->startStream()) {
		if(ImGui::BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("Failed to start audio stream! Error: %s", dac->getErrorText().c_str());
			if (ImGui::Button("OK", ImVec2(120, 0))) {ImGui::CloseCurrentPopup();}
			ImGui::EndPopup();
		}
		return;
	}
	state->playingAudio = true;
	while (state->playingAudio) {
		this_thread::sleep_for(chrono::milliseconds(100));
	}
	if (dac->isStreamOpen()) dac->closeStream();
}

static void mainApp(struct appstate* state) {
	ImGui::Begin("Wavd", NULL, ImGuiWindowFlags_NoCollapse | 
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | 
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | 
		ImGuiWindowFlags_MenuBar);

	if (state->loadFile) { load_file(state); }

	if (ImGui::BeginMainMenuBar()) {
		ImGui::MenuItem("Load File", NULL, &state->loadFile, !state->playingAudio);
		ImGui::EndMainMenuBar();
	}

	ImGui::SetNextWindowSizeConstraints(ImVec2(state->io.DisplaySize.x*0.15, 0), ImVec2(state->io.DisplaySize.x * 0.325, FLT_MAX));
	if (ImGui::BeginChild("child1", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_ResizeX)) {
		if (state->current_wave) {
			ImGui::Text("Filename: %ls", state->filename);
			ImGui::Text("Audio Format: %s", state->current_wave->file_data.fmt_chunk.audio_format == PCM_FORMAT ? "PCM" : "Unknown");
			ImGui::Text("Channels: %d", state->current_wave->file_data.fmt_chunk.num_channels);
			ImGui::Text("Sample Rate: %d hz", state->current_wave->file_data.fmt_chunk.sample_rate);
			ImGui::Text("Byte Rate: %d bytes/s", state->current_wave->file_data.fmt_chunk.byte_rate);
			ImGui::Text("Block Align: %d", state->current_wave->file_data.fmt_chunk.block_align);
			ImGui::Text("Bits Per Sample: %d bits", state->current_wave->file_data.fmt_chunk.bits_per_sample);
			ImGui::Text("Data Size: %d bytes", state->current_wave->file_data.wave_data.data_size);
			ImGui::Text("Current Address: 0x%p", state->current_addr);
			ImGui::Text("Audio playing: %s", state->playingAudio ? "Yes" : "No");
			ImGui::Text("Buffer Frames: %d", state->bufferFrames);

			char play_button_label[20];
			snprintf(play_button_label, sizeof(play_button_label), "%s Audio", state->playingAudio ? "Pause" : "Play");
			if (ImGui::Button(play_button_label, ImVec2(-1, 0))) {
				if (!state->playingAudio) {
					thread audio_thread(playAudio, state);
					audio_thread.detach();
				}
				else {
					state->playingAudio = false;
				}
			}

			mem_edit_2.DrawContents((short*)state->start_addr, state->end_addr-state->start_addr, state->start_addr);
		}
		else {
			ImGui::Text("No file loaded.");
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();

	if (ImGui::BeginChild("child2", ImVec2(0, 0), ImGuiChildFlags_Borders) && state->current_wave != NULL) {
		load_graphs(state);
	}
	ImGui::EndChild();
	
	ImGui::SetWindowSize(state->io.DisplaySize);
	ImGui::SetWindowPos(ImVec2(0, 0));
	ImGui::End();
}