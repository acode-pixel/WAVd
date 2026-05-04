#define SDL_MAIN_USE_CALLBACKS 1
#define IMGUI_DEFINE_MATH_OPERATORS
#include "RtAudio.h"
#include <cstdlib>
#include <fileapi.h>
#include <Windows.h>
#include <iostream>
#include <cstdio>
#include <thread>
#include <string>
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
	wchar_t current_file[MAX_PATH],
		current_playlist[MAX_PATH],
		last_error[256];
	wchar_t* filename;
	struct wave_file* current_wave;
	bool loadFile = false,
		playingAudio = false,
		makePlaylist = false,
		selectPlaylist = false,
		active;
	size_t start_addr = 0, 
		end_addr = 0, 
		current_addr = 0;
	double current_time = 0.0, 
		audio_dt = 0, 
		num_seconds = 0;
	int bufferFrames = 0,
		playlist_selected = 0;
	vector <char*> songs, 
		playlists;
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

static void log_Error(struct appstate* state, const wchar_t* message, DWORD id) {
	if (id) {
		wchar_t* error_msg = NULL;
		FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, id, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&error_msg, 0, NULL);
		swprintf(state->last_error, sizeof(state->last_error), L"%s. Error: %ls", message, error_msg);
		LocalFree(error_msg);
	}
	else {
		swprintf(state->last_error, sizeof(state->last_error), L"%s", message);
	}
}

struct wave_file* analyzeWAV(struct appstate* state, wchar_t* fileSelected)
{
	LPCWSTR wav_filepath = fileSelected;
	HANDLE file = CreateFileW(wav_filepath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		log_Error(state, L"Failed to open file.", GetLastError());
		return NULL;
	}

	HANDLE hmap = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
	if (hmap == NULL) {
		log_Error(state, L"Failed to create file mapping.", GetLastError());
		CloseHandle(file);
		return NULL;
	}

	LPVOID file_data = MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0);
	if (file_data == NULL) {
		log_Error(state, L"Failed to map view of file.", GetLastError());
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

static void load_file(struct appstate* state, wchar_t* file_to_load=NULL) {
	// close previous file if open
	if (state->current_wave) {
		UnmapViewOfFile(state->current_wave->mapped_data);
		CloseHandle(state->current_wave->hmap);
		CloseHandle(state->current_wave->file);
		free(state->current_wave);
		state->current_wave = NULL;
		state->current_time = 0;
	}

	if (file_to_load) {
		state->current_file[0] = '\0';
		wcscpy_s(state->current_file, file_to_load);
		state->current_wave = analyzeWAV(state, file_to_load);
		state->filename = state->current_file + lstrlenW(state->current_playlist) + 1;
		state->start_addr = (size_t)state->current_wave->file_data.wave_data.data_ptr;
		state->current_addr = state->start_addr;
		state->bufferFrames = state->current_wave->file_data.fmt_chunk.sample_rate;
		state->loadFile = false;
		return;
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
		state->current_file[0] = '\0';
		wcscpy_s(state->current_file, ofn.lpstrFile);
		state->filename = state->current_file + ofn.nFileOffset;
		state->current_wave = analyzeWAV(state, state->current_file);
		state->start_addr = (size_t)state->current_wave->file_data.wave_data.data_ptr;
		state->current_addr = state->start_addr;
		state->bufferFrames = state->current_wave->file_data.fmt_chunk.sample_rate;
	}
	else {
		wprintf(L"File selection cancelled.\n");
	}
	state->loadFile = false;
}

static void popup_Error(struct appstate* state) {
	if (ImGui::BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("%ls", state->last_error);
		if (ImGui::Button("OK", ImVec2(120, 0))) { 
			ImGui::CloseCurrentPopup(); 
			state->last_error[0] = '\0';
		}
		ImGui::EndPopup();
	}
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
		log_Error(state, L"No audio devices found!", 0);
		return;
	}
	RtAudio::StreamParameters parameters;
	parameters.deviceId = dac->getDefaultOutputDevice();
	parameters.nChannels = state->current_wave->file_data.fmt_chunk.num_channels;
	parameters.firstChannel = 0;
	double sampleRate = state->current_wave->file_data.fmt_chunk.sample_rate;
	unsigned int bufferFrames = state->bufferFrames;

	if (dac->openStream(&parameters, NULL, state->current_wave->file_data.fmt_chunk.bytes_per_sample, sampleRate, &bufferFrames, &saw, state)) {
		log_Error(state, L"Failed to open audio stream!", 0);
		return;
	}

	if (dac->startStream()) {
		wchar_t error_message[256];
		swprintf(error_message, sizeof(error_message), L"Failed to start audio stream! Error: %ls", dac->getErrorText().c_str());
		log_Error(state, error_message, 0);
		return;
	}
	state->playingAudio = true;
	while (state->playingAudio) {
		this_thread::sleep_for(chrono::milliseconds(100));
	}
	if (dac->isStreamOpen()) dac->closeStream();
}

static void make_playlist(struct appstate* state) {
	// create folder in same directory as executable called "playlist"
	 if(!CreateDirectoryW(L"playlist", NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		 log_Error(state, L"Failed to create playlist directory", GetLastError());
	 }

	 if (ImGui::BeginPopupModal("Name of Playlist", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		 static char playlist_name[50] = "My Playlist";
		 ImGui::InputText("Playlist Name", playlist_name, sizeof(playlist_name));
		 if (ImGui::Button("Create", ImVec2(120, 0))) {
			 wchar_t playlist_path[MAX_PATH];
			 swprintf(playlist_path, MAX_PATH, L"playlist\\%S", playlist_name);
			 if(CreateDirectoryW(playlist_path, NULL)) {
				 wmemcpy(state->current_playlist, playlist_path, MAX_PATH);
				 ImGui::CloseCurrentPopup();
			 }
			 else {
				 log_Error(state, L"Failed to create playlist directory", GetLastError());
			 }
			 state->makePlaylist = false;
		 }
		 ImGui::SameLine();
		 if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			 ImGui::CloseCurrentPopup();
			 state->makePlaylist = false;
		 }
		 ImGui::EndPopup();
	 }
}

static void select_playlist(struct appstate* state) {
	wchar_t dirPath[MAX_PATH] = L"playlist";
	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW(L"playlist\\*", &findData);
	if (hFind == INVALID_HANDLE_VALUE) {
		log_Error(state, L"Failed to open directory", GetLastError());
		return;
	}

	for(int i = 0; i < state->playlists.size(); i++) {
		free(state->playlists[i]);
	}
	vector<char*> playlists;
	int default_index = -1;
	do {
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (wcscmp(findData.cFileName, L".") != 0 && wcscmp(findData.cFileName, L"..") != 0) {
				char playlist_name[MAX_PATH];
				size_t converted_chars = 0;
				wcstombs_s(&converted_chars, playlist_name, findData.cFileName, MAX_PATH);
				playlists.insert(playlists.end(), _strdup(playlist_name));
			}
		}
	} while (FindNextFileW(hFind, &findData));
	FindClose(hFind);

	state->playlists.swap(playlists);

	if(ImGui::BeginPopup("Select Playlist", ImGuiWindowFlags_NoMove)) {
		for (size_t i = 0; i < state->playlists.size(); i++) {
			if (ImGui::Selectable(state->playlists[i], default_index == (int)i)) {
				wchar_t playlist_path[MAX_PATH];
				swprintf(playlist_path, MAX_PATH, L".\\playlist\\%S", state->playlists[i]);
				wmemcpy(state->current_playlist, playlist_path, MAX_PATH);
			}
		}
		state->selectPlaylist = false;
		ImGui::EndPopup();
	}
}

static void refresh_songs(struct appstate* state) {
	for (size_t i = 0; i < state->songs.size(); i++) {
		free(state->songs[i]);
	}

	// list files in playlist directory
	wchar_t dirPath[MAX_PATH];
	swprintf(dirPath, MAX_PATH, L"%ls\\*", state->current_playlist);
	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW(dirPath, &findData);
	if (hFind == INVALID_HANDLE_VALUE) {
		log_Error(state, L"Failed to open directory", GetLastError());
		ImGui::EndTabItem();
		return;
	}
	int i = 0;
	vector<char*> songs;
	while (FindNextFileW(hFind, &findData)) {
		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			char* song_name = (char*)malloc(sizeof(char) * MAX_PATH + 1);
			size_t converted = 0;
			wcstombs_s(&converted, song_name, MAX_PATH, findData.cFileName, MAX_PATH);
			songs.push_back(song_name);
			i++;
		}
	}
	FindClose(hFind);
	state->songs.swap(songs);
}

static void mainApp(struct appstate* state) {
	ImGui::Begin("Wavd", NULL, ImGuiWindowFlags_NoCollapse | 
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | 
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | 
		ImGuiWindowFlags_MenuBar);

	if (state->loadFile) { load_file(state); }
	make_playlist(state);
	select_playlist(state);
	popup_Error(state);
	if (state->last_error[0] != '\0') { ImGui::OpenPopup("Error"); }
	if (state->makePlaylist) { ImGui::OpenPopup("Name of Playlist"); }
	if (state->selectPlaylist) { ImGui::OpenPopup("Select Playlist"); }

	if (ImGui::BeginMainMenuBar()) {
		ImGui::MenuItem("Load File", NULL, &state->loadFile, !state->playingAudio);
		ImGui::MenuItem("Make Playlist", NULL, &state->makePlaylist, !state->playingAudio);
		ImGui::MenuItem("Select Playlist", NULL, &state->selectPlaylist, !state->playingAudio);
		ImGui::EndMainMenuBar();
	}

	ImGui::SetNextWindowSizeConstraints(ImVec2(state->io.DisplaySize.x*0.15, 0), ImVec2(state->io.DisplaySize.x * 0.325, FLT_MAX));
	if (ImGui::BeginChild("child1", ImVec2(0, 0), ImGuiChildFlags_ResizeX, ImGuiWindowFlags_NoScrollbar)) {
		if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
			if (ImGui::BeginTabItem("File Info")) {
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

					mem_edit_2.DrawContents((short*)state->start_addr, state->end_addr - state->start_addr, state->start_addr);
				}
				else {
					ImGui::Text("No file loaded.");
				}
				ImGui::EndTabItem();
			}

			if(lstrlenW(state->current_playlist) > 0) {
				if (ImGui::BeginTabItem("Playlist")) {
					refresh_songs(state);
					ImGui::Text("Current Playlist: %ls", state->current_playlist);
					ImGui::Text("Current song: %s", state->songs.size() > 0 ? state->songs[state->playlist_selected] : "None");
					if(ImGui::Button("Add Song", ImVec2(-1, 0))) {
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
							wchar_t dest_path[MAX_PATH];
							swprintf(dest_path, MAX_PATH, L"%ls\\%ls", state->current_playlist, ofn.lpstrFile + ofn.nFileOffset);
							if (!CopyFileW(ofn.lpstrFile, dest_path, FALSE)) {
								log_Error(state, L"Failed to add song to playlist!", GetLastError());
							}
						}
					}


					if (ImGui::BeginListBox("##playlist_songs", ImVec2(-1, -1))) {
						for (size_t i = 0; i < state->songs.size(); i++) {
							if (ImGui::Selectable(state->songs[i], state->playlist_selected == i)) {
								state->playlist_selected = i;
								wchar_t full_path[MAX_PATH];
								WCHAR wide_song[MAX_PATH];
								mbstowcs_s(NULL, wide_song, MAX_PATH, state->songs[i], MAX_PATH);
								swprintf(full_path, MAX_PATH, L"%ls\\%ls", state->current_playlist, wide_song);
								load_file(state, full_path);
							}
						}
						ImGui::EndListBox();
					}

					ImGui::EndTabItem();
				}
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();

	if (ImGui::BeginChild("child2", ImVec2(0, 0)) && state->current_wave != NULL) {
		load_graphs(state);
	}
	ImGui::EndChild();
	
	ImGui::SetWindowSize(state->io.DisplaySize);
	ImGui::SetWindowPos(ImVec2(0, 0));
	ImGui::End();
}