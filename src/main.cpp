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
extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavutil/frame.h>
	#include <libavutil/mem.h>
	#include <libavformat/avformat.h>
	#include <libswresample/swresample.h>
}

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096
#define MP3_FILE 1
#define WAV_FILE 0
#define WAV_PCM 1
#define WAV_FLT 3

using namespace std;

struct format_chunk {
	unsigned short sample_format;
	unsigned short num_channels;
	unsigned int sample_rate;
	unsigned int byte_rate;
	unsigned short block_align;
	unsigned short bits_per_sample;
	unsigned int bytes_per_sample;
};

struct wave_data {
	unsigned int data_size;
	void* data_ptr;
};

struct waveform {
	struct format_chunk fmt_chunk;
	struct wave_data wave_data;
};

struct mp3_data {
	bool crc_check,
		is_copyrighted,
		is_original;
	int mpeg_version;
	int layer;
	int bitrate;
	int sample_rate;
	int padding;
	int channel_mode;
	int mode_extension;
	size_t data_size;
	struct waveform waveform;
	HANDLE pcm_file;
	HANDLE pcm_hmap;
};

union Data {
	struct waveform wav_file_data;
	struct mp3_data mp3_file_data;
};

struct sound_file {
	HANDLE file;
	HANDLE hmap;
	int MEDIA_TYPE;
	LPVOID mapped_data;
	Data file_data;
	bool is_planar;
};

struct appstate {
	SDL_Window* window;
	SDL_GLContext gl_context;
	ImGuiIO io;
	wchar_t current_file[MAX_PATH],
		current_playlist[MAX_PATH],
		last_error[256],
		info_msg[256];
	wchar_t* filename;
	struct sound_file* current_sound;
	bool loadFile = false,
		playingAudio = false,
		makePlaylist = false,
		selectPlaylist = false,
		playingPlaylist = false,
		proccessingAudio = false,
		active;
	size_t start_addr = 0, 
		end_addr = 0, 
		current_addr = 0;
	double current_time = 0.0, 
		audio_dt = 0, 
		num_seconds = 0;
	int bufferFrames = 0,
		playlist_selected = 0;
	double proccessingProgress = 0;
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

static void writeSound_s16bit(struct appstate* state, struct waveform* waveform, short* buffer, short* data, unsigned int nBufferFrames) {
	short* buf = buffer;
	for (unsigned int i = 0; i < nBufferFrames; i++) {
		for (unsigned int channel = 0; channel < waveform->fmt_chunk.num_channels; channel++) {
			if ((int)state->current_time < (int)state->num_seconds) {
				*buf = *data;
				buf++;
				data++;
				state->current_addr += waveform->fmt_chunk.bytes_per_sample;
			}
			else {
				state->playingAudio = false;
				*buf = 0.0;
				buf++;
			}
		}
	}
}

static void writeSound_f32bit(struct appstate* state, struct waveform* waveform, float* buffer, float* data, unsigned int nBufferFrames) {
	float* buf = buffer;

	for (unsigned int i = 0; i < nBufferFrames; i++) {
		for (unsigned int channel = 0; channel < waveform->fmt_chunk.num_channels; channel++) {
			if ((int)state->current_time < (int)state->num_seconds) {
				*buf = *data ;
				buf++;
				data++;
				state->current_addr += waveform->fmt_chunk.bytes_per_sample;
			}
			else {
				state->playingAudio = false;
				*buf = 0;
				buf++;
			}
		}
	}
}

static void writeSound_s32bit(struct appstate* state, struct waveform* waveform, int* buffer, int* data, unsigned int nBufferFrames) {
	int* buf = buffer;
	for (unsigned int i = 0; i < nBufferFrames; i++) {
		for (unsigned int channel = 0; channel < waveform->fmt_chunk.num_channels; channel++) {
			if ((int)state->current_time < (int)state->num_seconds) {
				*buf = *data;
				buf++;
				data++;
				state->current_addr += waveform->fmt_chunk.bytes_per_sample;
			}
			else {
				state->playingAudio = false;
				*buf = 0.0;
				buf++;
			}
		}
	}
}

static int outputSound(void* outputBuffer, void* inputBuffer, unsigned int nBufferFrames,
	double streamTime, RtAudioStreamStatus status, void* userData)
{
	struct appstate* state = (struct appstate*)userData;
	struct waveform* current_wave = NULL;

	if(state->current_sound->MEDIA_TYPE == WAV_FILE) {
		current_wave = &state->current_sound->file_data.wav_file_data;
	}
	else if(state->current_sound->MEDIA_TYPE == MP3_FILE) {
		current_wave = &state->current_sound->file_data.mp3_file_data.waveform;
	}

	void* buffer = outputBuffer;
	void* data = (void*)state->current_addr;

	switch (current_wave->fmt_chunk.sample_format) {
		case AV_SAMPLE_FMT_S16:
			writeSound_s16bit(state, current_wave, (short*)buffer, (short*)data, nBufferFrames);
			break;
		case AV_SAMPLE_FMT_S32:
			writeSound_s32bit(state, current_wave, (int*)buffer, (int*)data, nBufferFrames);
			break;
		case AV_SAMPLE_FMT_FLT:
		case AV_SAMPLE_FMT_FLTP:
			writeSound_f32bit(state, current_wave, (float*)buffer, (float*)data, nBufferFrames);
			break;
	}

	state->audio_dt = nBufferFrames / (double)current_wave->fmt_chunk.sample_rate;
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

static int decode(AVCodecContext* dec_ctx, AVPacket* pkt, AVFrame* frame, FILE* wavFile) {
	int ret = avcodec_send_packet(dec_ctx, pkt);
	if (ret < 0) {
		return ret;
	}

	while (ret >= 0) {
		ret = avcodec_receive_frame(dec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		else if (ret < 0) {
			return ret;
		}
		int data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
		if (data_size < 0) {
			return -1;
		}
		for (int i = 0; i < frame->nb_samples; i++)
			for (int ch = 0; ch < dec_ctx->ch_layout.nb_channels; ch++)
				fwrite(frame->data[ch] + data_size * i, 1, data_size, wavFile);
	}
}

static int getPCM(struct appstate* state, struct sound_file* sound_file) {
	if (sound_file->MEDIA_TYPE == MP3_FILE) {
		FILE* wavFile = NULL;
		fopen_s(&wavFile, "temp.pcm", "wb");

		HANDLE hmap = sound_file->hmap;
		if (hmap == NULL) {
			log_Error(state, L"Failed to create file mapping.", GetLastError());
			return -1;
		}

		uint8_t* file_data = (uint8_t*)sound_file->mapped_data;
		if (file_data == NULL) {
			log_Error(state, L"Failed to map view of file.", GetLastError());
			return -1;
		}

		const AVCodec* codec;
		AVCodecContext* c = NULL;
		AVCodecParserContext* parser = NULL;
		uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
		AVPacket* avpkt;
		size_t   file_size;
		AVFrame* decoded_frame = NULL;

		avpkt = av_packet_alloc();

		codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
		parser = av_parser_init(codec->id);
		c = avcodec_alloc_context3(codec);


		if (avcodec_open2(c, codec, NULL) < 0) {
			log_Error(state, L"Could not open codec.", 0);
			return -1;
		}

		file_size = sound_file->file_data.mp3_file_data.data_size;

		int start = SDL_GetPerformanceCounter();
		int freq = SDL_GetPerformanceFrequency();
		double loop_avg = 0;
		int loop_count = 0;

		while (file_size > 0) {
			loop_count++;
			int start_loop = SDL_GetPerformanceCounter();
			int ret;
			if (!decoded_frame) {
				if (!(decoded_frame = av_frame_alloc())) {
					log_Error(state, L"Could not allocate audio frame.", 0);
					return -1;
				}
			}
			memcpy_s(inbuf, AUDIO_INBUF_SIZE, file_data, FFMIN(file_size, AUDIO_INBUF_SIZE));
			ret = av_parser_parse2(parser, c, &avpkt->data, &avpkt->size,
				inbuf, FFMIN(file_size, AUDIO_INBUF_SIZE),
				AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			if (ret < 0) {
				log_Error(state, L"Error while parsing.", 0);
				return -1;
			}
			file_data += ret;
			file_size -= ret; 
			state->proccessingProgress = (double)(sound_file->file_data.mp3_file_data.data_size - file_size) / sound_file->file_data.mp3_file_data.data_size;
			swprintf(state->info_msg, sizeof(state->info_msg), L"Loading file progress: %.2f%%", state->proccessingProgress * 100);

			if (avpkt->size) {
				ret = decode(c, avpkt, decoded_frame, wavFile);
				if (ret < 0) {
					if (AVERROR(EAGAIN) == ret)
						log_Error(state, L"Error while decoding (EAGAIN).", 0);
					else if (AVERROR_EOF == ret)
						log_Error(state, L"Error while decoding (EOF).", 0);
					else if (AVERROR(EINVAL) == ret)
						log_Error(state, L"Error while decoding (EINVAL).", 0);
					else if (AVERROR(ENOMEM) == ret)
						log_Error(state, L"Error while decoding (ENOMEM).", 0);
					return -1;
				}
			}

			if (file_size == 0) {
				avpkt->data = NULL;
				avpkt->size = 0;
				ret = decode(c, avpkt, decoded_frame, wavFile);
				if (ret < 0) {
					log_Error(state, L"Error during flushing.", 0);
					return -1;
				}
			}
			av_frame_free(&decoded_frame);
			int end_loop = SDL_GetPerformanceCounter();
			double loop_time = (end_loop - start_loop) / (double)freq;
			loop_avg = loop_avg ? (loop_avg + loop_time) / 2 : loop_time;
		}
		int end = SDL_GetPerformanceCounter();
		double elapsed_time = (end - start) / (double)freq;
		printf("Decoding took %.2f seconds.\n", elapsed_time);
		printf("Average loop time: %.2f seconds.\n", loop_avg);
		printf("Total loops: %d\n", loop_count);

		fclose(wavFile);

		av_parser_close(parser);
		av_frame_free(&decoded_frame);
		av_packet_free(&avpkt);

		printf("MP3 file decoded and saved as PCM successfully.\n");

		struct waveform* waveform = &sound_file->file_data.mp3_file_data.waveform;
		waveform->fmt_chunk.sample_format = c->sample_fmt;
		waveform->fmt_chunk.num_channels = sound_file->file_data.mp3_file_data.channel_mode == 3 ? 1 : 2;
		waveform->fmt_chunk.sample_rate = sound_file->file_data.mp3_file_data.sample_rate;
		waveform->fmt_chunk.bits_per_sample = av_get_bytes_per_sample(c->sample_fmt) * 8;
		waveform->fmt_chunk.bytes_per_sample = av_get_bytes_per_sample(c->sample_fmt);
		waveform->fmt_chunk.byte_rate = waveform->fmt_chunk.sample_rate * waveform->fmt_chunk.num_channels * waveform->fmt_chunk.bytes_per_sample;
		waveform->fmt_chunk.block_align = (waveform->fmt_chunk.num_channels * waveform->fmt_chunk.bits_per_sample)/8;
		if(av_sample_fmt_is_planar(c->sample_fmt))
			sound_file->is_planar = true;

		avcodec_free_context(&c);

		HANDLE pcmFile = CreateFileA("temp.pcm", GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
		if (pcmFile == INVALID_HANDLE_VALUE) {
			log_Error(state, L"Failed to open PCM file.", GetLastError());
			return -1;
		}
		HANDLE pcmMap = CreateFileMappingA(pcmFile, NULL, PAGE_READONLY, 0, 0, NULL);
		if (pcmMap == NULL) {
			log_Error(state, L"Failed to create file mapping for PCM file.", GetLastError());
			CloseHandle(pcmFile);
			return -1;
		}
		LPVOID pcmData = MapViewOfFile(pcmMap, FILE_MAP_READ, 0, 0, 0);
		if (pcmData == NULL) {
			log_Error(state, L"Failed to map view of PCM file.", GetLastError());
			CloseHandle(pcmMap);
			CloseHandle(pcmFile);
			return -1;
		}

		waveform->wave_data.data_size = GetFileSize(pcmFile, NULL);
		waveform->wave_data.data_ptr = pcmData;

		sound_file->file_data.mp3_file_data.pcm_file = pcmFile;
		sound_file->file_data.mp3_file_data.pcm_hmap = pcmMap;
	}
}

struct sound_file* analyzeWAV(struct appstate* state, wchar_t* fileSelected)
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

	struct sound_file* wav_file = (struct sound_file*)malloc(sizeof(struct sound_file));
	struct waveform* current_wave = &wav_file->file_data.wav_file_data;
	int extra_bytes_to_skip = 0;
	ZeroMemory(wav_file, sizeof(struct sound_file));
	wav_file->file = file;
	wav_file->hmap = hmap;
	wav_file->mapped_data = file_data;

	current_wave->fmt_chunk.sample_format = *(unsigned short*)((char*)file_data + 20);
	current_wave->fmt_chunk.num_channels = *(unsigned short*)((char*)file_data + 22);
	current_wave->fmt_chunk.sample_rate = *(unsigned int*)((char*)file_data + 24);
	current_wave->fmt_chunk.byte_rate = *(unsigned int*)((char*)file_data + 28);
	current_wave->fmt_chunk.block_align = *(unsigned short*)((char*)file_data + 32);
	current_wave->fmt_chunk.bits_per_sample = *(unsigned short*)((char*)file_data + 34);
	current_wave->fmt_chunk.bytes_per_sample = current_wave->fmt_chunk.bits_per_sample / 8;
	if (current_wave->fmt_chunk.sample_format == WAV_PCM) {
		current_wave->fmt_chunk.sample_format = current_wave->fmt_chunk.bits_per_sample == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
	} else if (current_wave->fmt_chunk.sample_format == WAV_FLT) {
		current_wave->fmt_chunk.sample_format = AV_SAMPLE_FMT_FLT;
	}

	if (strncmp((char*)file_data+36, "LIST", 4) == 0) {
		extra_bytes_to_skip = *(unsigned int*)((char*)file_data + 40) + 8;
	}

	current_wave->wave_data.data_size = *(unsigned int*)((char*)file_data + 36 + extra_bytes_to_skip + 4);
	current_wave->wave_data.data_ptr = (short int*)((char*)file_data + 36 + extra_bytes_to_skip + 8);
	return wav_file;
}

struct sound_file* analyzeMP3(struct appstate* state, char* fileSelected) {
	HANDLE mp3File = CreateFileA(fileSelected, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (mp3File == INVALID_HANDLE_VALUE) {
		log_Error(state, L"Failed to open file.", GetLastError());
		return NULL;
	}
	HANDLE hmap = CreateFileMappingW(mp3File, NULL, PAGE_READONLY, 0, 0, NULL);
	if (hmap == NULL) {
		log_Error(state, L"Failed to create file mapping.", GetLastError());
		CloseHandle(mp3File);
		return NULL;
	}
	uint8_t* file_data = (uint8_t*)MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0);
	if (file_data == NULL) {
		log_Error(state, L"Failed to map view of file.", GetLastError());
		CloseHandle(hmap);
		CloseHandle(mp3File);
		return NULL;
	}

	int id3v2_size = 0;
	if (strncmp((char*)file_data, "ID3", 3) == 0) {
		id3v2_size = (*((char*)file_data + 6) & 0x7F) << 21 | (*((char*)file_data + 7) & 0x7F) << 14 | (*((char*)file_data + 8) & 0x7F) << 7 | (*((char*)file_data + 9) & 0x7F);
		file_data += 10 + id3v2_size;
	}

	struct mp3_data* mp3_info = (struct mp3_data*)malloc(sizeof(struct mp3_data));
	ZeroMemory(mp3_info, sizeof(struct mp3_data));
	mp3_info->crc_check = (*((char*)file_data + 1) & 0x01) == 0;
	mp3_info->mpeg_version = ((*((char*)file_data + 1) >> 3) & 0x03);
	mp3_info->layer = ((*((char*)file_data + 1) >> 1) & 0x03);
	mp3_info->padding = ((*((char*)file_data + 2) >> 1) & 0x01);
	mp3_info->channel_mode = ((*((char*)file_data + 3) >> 6) & 0x03);
	mp3_info->mode_extension = ((*((char*)file_data + 3) >> 4) & 0x03);
	int bitrate_index = ((*((char*)file_data + 2) >> 4) & 0x0F);
	int sample_rate_index = ((*((char*)file_data + 2) >> 2) & 0x03);
	int bitrate_table[2][3][16] = {
		{ // MPEG Version 1
			{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1}, // Layer I
			{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1}, // Layer II
			{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, -1} // Layer III
		},
		{ // MPEG Version 2 & 2.5
			{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1}, // Layer I
			{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1}, // Layer II
			{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, -1} // Layer III
		}
	};
	int sample_rate_table[4][3] = {
		{44100, 48000, 32000}, // MPEG Version 1
		{22050, 24000, 16000}, // MPEG Version 2
		{0, 0, 0}, // Reserved
		{11025, 12000, 8000} // MPEG Version 2.5
	};
	mp3_info->bitrate = bitrate_table[mp3_info->mpeg_version == 3 ? 0 : 1][mp3_info->layer - 1][bitrate_index];
	mp3_info->sample_rate = sample_rate_table[3 - mp3_info->mpeg_version][sample_rate_index];
	mp3_info->is_copyrighted = ((*((char*)file_data + 3) >> 2) & 0x02) != 0;
	mp3_info->is_original = ((*((char*)file_data + 3) >> 2) & 0x01) != 0;
	mp3_info->data_size = GetFileSize(mp3File, NULL) - id3v2_size;

	struct sound_file* mp3_file = (struct sound_file*)malloc(sizeof(struct sound_file));
	mp3_file->file = mp3File;
	mp3_file->hmap = hmap;
	mp3_file->mapped_data = file_data;
	mp3_file->MEDIA_TYPE = MP3_FILE;
	memcpy(&mp3_file->file_data.mp3_file_data, mp3_info, sizeof(struct mp3_data));
	free(mp3_info);

	getPCM(state, mp3_file);

	return mp3_file;

}

static void load_file(struct appstate* state, wchar_t* file_to_load=NULL) {
	// close previous file if open
	if (state->current_sound) {
		UnmapViewOfFile(state->current_sound->mapped_data);
		CloseHandle(state->current_sound->hmap);
		CloseHandle(state->current_sound->file);

		if (state->current_sound->MEDIA_TYPE == MP3_FILE) {
			UnmapViewOfFile(state->current_sound->file_data.mp3_file_data.waveform.wave_data.data_ptr);
			CloseHandle(state->current_sound->file_data.mp3_file_data.pcm_hmap);
			CloseHandle(state->current_sound->file_data.mp3_file_data.pcm_file);
		}

		free(state->current_sound);
		state->current_sound = NULL;
		state->current_time = 0;
	}

	swprintf(state->info_msg, sizeof(state->info_msg), L"Loading file progress: 0%%");

	if (file_to_load) {
		state->current_file[0] = '\0';
		wcscpy_s(state->current_file, file_to_load);
		state->current_sound = analyzeWAV(state, file_to_load);
		state->filename = state->current_file + lstrlenW(state->current_playlist) + 1;
		state->start_addr = (size_t)state->current_sound->file_data.wav_file_data.wave_data.data_ptr;
		state->current_addr = state->start_addr;
		state->bufferFrames = state->current_sound->file_data.wav_file_data.fmt_chunk.sample_rate;
		state->loadFile = false;
		state->proccessingAudio = false;
		state->info_msg[0] = '\0';
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
	ofn.lpstrFilter = L"WAV Files\0*.wav\0MP3 Files\0*.mp3\0All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameW(&ofn)) {
		state->current_file[0] = '\0';
		wcscpy_s(state->current_file, ofn.lpstrFile);
		state->filename = state->current_file + ofn.nFileOffset;
		char fileSelected[MAX_PATH];
		size_t convertedChars = 0;
		wcstombs_s(&convertedChars, fileSelected, sizeof(fileSelected), state->current_file, _TRUNCATE);
		if (strcmp(fileSelected + strlen(fileSelected) - 4, ".mp3") == 0) {
			state->current_sound = analyzeMP3(state, fileSelected);
			if (state->current_sound == NULL) {
				log_Error(state, L"Failed to analyze MP3 file.", 0);
				state->loadFile = false;
				state->proccessingAudio = false;
				state->info_msg[0] = '\0';
				return;
			}
			state->start_addr = (size_t)state->current_sound->file_data.mp3_file_data.waveform.wave_data.data_ptr;
			state->bufferFrames = state->current_sound->file_data.mp3_file_data.waveform.fmt_chunk.sample_rate;
		}
		else {
			state->current_sound = analyzeWAV(state, state->current_file);
			if (state->current_sound == NULL) {
				state->loadFile = false;
				state->proccessingAudio = false;
				log_Error(state, L"Failed to analyze WAV file.", 0);
				state->info_msg[0] = '\0';
				return;
			}
			state->start_addr = (size_t)state->current_sound->file_data.wav_file_data.wave_data.data_ptr;
			state->bufferFrames = state->current_sound->file_data.wav_file_data.fmt_chunk.sample_rate;
		}

		state->current_addr = state->start_addr;
	}
	else {
		wprintf(L"File selection cancelled.\n");
	}
	state->loadFile = false;
	state->proccessingAudio = false;
	state->info_msg[0] = '\0';

	return;
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

static void popup_Info(struct appstate* state) {
	if (ImGui::BeginPopupModal("Info", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (state->info_msg[0] != '\0')
			ImGui::Text("%ls", state->info_msg);
		else 
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

static void load_graphs(struct appstate* state) {
	struct waveform* current_wave = NULL;
	int AxisLimits_min, AxisLimits_max;

	if(state->current_sound->MEDIA_TYPE == WAV_FILE) {
		current_wave = &state->current_sound->file_data.wav_file_data;
	}
	else if(state->current_sound->MEDIA_TYPE == MP3_FILE) {
		current_wave = &state->current_sound->file_data.mp3_file_data.waveform;
	}

	switch (current_wave->fmt_chunk.sample_format) {
		case AV_SAMPLE_FMT_S16:
			AxisLimits_min = -32768;
			AxisLimits_max = 32767;
			break;
		case AV_SAMPLE_FMT_S32:
			AxisLimits_min = -2147483648;
			AxisLimits_max = 2147483647;
			break;
		case AV_SAMPLE_FMT_FLT:
		case AV_SAMPLE_FMT_FLTP:
			AxisLimits_min = -1;
			AxisLimits_max = 1;
			break;
	}



	if (ImPlot::BeginSubplots("Waveform", (int)current_wave->fmt_chunk.num_channels, 1, ImVec2(-1, -1), ImPlotSubplotFlags_LinkCols)) {
		for (int channel = 0; channel < current_wave->fmt_chunk.num_channels; channel++) {
			char buf[50];
			snprintf(buf, sizeof(buf), "Channel %d", channel + 1);
			ImPlot::BeginPlot(buf, ImVec2(-1, -1));
			ImPlot::SetupAxes("Time (s)", "Amplitude");
			ImPlot::SetupAxisLimits(ImAxis_Y1, AxisLimits_min, AxisLimits_max, ImPlotCond_Always);
			if (state->playingAudio)
				ImPlot::SetupAxisLimits(ImAxis_X1, state->current_time - state->audio_dt, state->current_time, ImPlotCond_Always);

			int num_samples = current_wave->wave_data.data_size / (current_wave->fmt_chunk.bytes_per_sample);
			num_samples = num_samples / current_wave->fmt_chunk.num_channels;
			state->num_seconds = (double)num_samples / current_wave->fmt_chunk.sample_rate;

			ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, state->num_seconds);

			int downsample = (int)(ImPlot::GetPlotLimits().X.Size()) < 1 ? 1 : (int)(ImPlot::GetPlotLimits().X.Size());
			int start = (int)(ImPlot::GetPlotLimits().X.Min * current_wave->fmt_chunk.sample_rate);
			start = start < 0 ? 0 : start > num_samples - 1 ? num_samples - 1 : start;
			int end = (int)(ImPlot::GetPlotLimits().X.Max * current_wave->fmt_chunk.sample_rate);
			end = end < 0 ? 0 : end > num_samples - 1 ? num_samples - 1 : end;
			long int size = (end - start) / downsample;
			void* audio_data = current_wave->wave_data.data_ptr;

			ImGui::Text("Displaying samples %d to %d (downsampled by %d)", start, end, downsample);
			ImPlot::DragLineX(0, &state->current_time, ImVec4(1, 0, 0, 1), 1.0);
			state->start_addr = (size_t)audio_data + (start * current_wave->fmt_chunk.block_align) + (channel * current_wave->fmt_chunk.bytes_per_sample);

			switch (current_wave->fmt_chunk.sample_format) {
			case AV_SAMPLE_FMT_S16:
				ImPlot::PlotLine("Audio Data", (short*)state->start_addr, size, 1.0 * downsample / current_wave->fmt_chunk.sample_rate, ImPlot::GetPlotLimits().X.Min, { ImPlotProp_Stride, current_wave->fmt_chunk.block_align * downsample});
				break;
			case AV_SAMPLE_FMT_S32:
				ImPlot::PlotLine("Audio Data", (int*)state->start_addr, size, 1.0 * downsample / current_wave->fmt_chunk.sample_rate, ImPlot::GetPlotLimits().X.Min, { ImPlotProp_Stride, current_wave->fmt_chunk.block_align * downsample });
				break;
			case AV_SAMPLE_FMT_FLTP:
			case AV_SAMPLE_FMT_FLT:
				ImPlot::PlotLine("Audio Data", (float*)state->start_addr, size, 1.0 * downsample / current_wave->fmt_chunk.sample_rate, ImPlot::GetPlotLimits().X.Min, { ImPlotProp_Stride, current_wave->fmt_chunk.block_align * downsample });
				break;
			}

			state->current_time = state->current_time < 0 ? 0 : state->current_time > state->num_seconds ? state->num_seconds : state->current_time;

			ImPlot::TagX(state->current_time, ImVec4(1, 0, 0, 1), "%.3f", state->current_time);

			state->end_addr = (size_t)audio_data + (end * current_wave->fmt_chunk.block_align) + (channel * current_wave->fmt_chunk.bytes_per_sample);
			state->current_addr = (size_t)audio_data + (int)(state->current_time * current_wave->fmt_chunk.sample_rate) * current_wave->fmt_chunk.block_align + (channel * current_wave->fmt_chunk.bytes_per_sample);
			state->bufferFrames = (int)((ImPlot::GetPlotLimits().X.Max - ImPlot::GetPlotLimits().X.Min) * current_wave->fmt_chunk.sample_rate);
			state->bufferFrames = state->bufferFrames < current_wave->fmt_chunk.sample_rate ? state->bufferFrames : current_wave->fmt_chunk.sample_rate;
			state->bufferFrames = state->bufferFrames > 0 ? state->bufferFrames : current_wave->fmt_chunk.sample_rate;
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

	struct waveform* current_wave = NULL;
	if(state->current_sound->MEDIA_TYPE == WAV_FILE) {
		current_wave = &state->current_sound->file_data.wav_file_data;
	}
	else if(state->current_sound->MEDIA_TYPE == MP3_FILE) {
		current_wave = &state->current_sound->file_data.mp3_file_data.waveform;
	}

	RtAudio::StreamParameters parameters;
	parameters.deviceId = dac->getDefaultOutputDevice();
	parameters.nChannels = current_wave->fmt_chunk.num_channels;
	parameters.firstChannel = 0;
	double sampleRate = current_wave->fmt_chunk.sample_rate;
	unsigned int bufferFrames = state->bufferFrames;
	int audio_format;

	switch (current_wave->fmt_chunk.sample_format) {
		case AV_SAMPLE_FMT_S16:
			audio_format = RTAUDIO_SINT16;
			break;
		case AV_SAMPLE_FMT_S32:
			audio_format = RTAUDIO_SINT32;
			break;
		case AV_SAMPLE_FMT_FLTP:
		case AV_SAMPLE_FMT_FLT:
			audio_format = RTAUDIO_FLOAT32;
			break;
		default:
			log_Error(state, L"Unsupported audio format!", 0);
			return;
	}

	RtAudio::StreamOptions options;
	options.flags = RTAUDIO_HOG_DEVICE | RTAUDIO_SCHEDULE_REALTIME;

	if (dac->openStream(&parameters, NULL, audio_format, sampleRate, &bufferFrames, &outputSound, state, &options)) {
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

static void showSoundInfo(struct appstate* state) {
	struct waveform* current_wave = NULL;
	if(state->current_sound->MEDIA_TYPE == WAV_FILE) {
		current_wave = &state->current_sound->file_data.wav_file_data;
		ImGui::Text("Audio Format: %s", "WAV");
		ImGui::Text("Channels: %d", current_wave->fmt_chunk.num_channels);
		ImGui::Text("Sample Rate: %d hz", current_wave->fmt_chunk.sample_rate);
		ImGui::Text("Byte Rate: %d bytes/s", current_wave->fmt_chunk.byte_rate);
		ImGui::Text("Block Align: %d", current_wave->fmt_chunk.block_align);
		ImGui::Text("Bits Per Sample: %d bits", current_wave->fmt_chunk.bits_per_sample);
		ImGui::Text("Data Size: %d bytes", current_wave->wave_data.data_size);
		ImGui::Text("Buffer Frames: %d frames", state->bufferFrames);
	}
	else if(state->current_sound->MEDIA_TYPE == MP3_FILE) {
		current_wave = &state->current_sound->file_data.mp3_file_data.waveform;
		ImGui::Text("Audio Format: %s", "MP3");
		ImGui::Text("Channels: %d", current_wave->fmt_chunk.num_channels);
		ImGui::Text("Sample Rate: %d hz", current_wave->fmt_chunk.sample_rate);
		ImGui::Text("Byte Rate: %d bytes/s", current_wave->fmt_chunk.byte_rate);
		ImGui::Text("Block Align: %d", current_wave->fmt_chunk.block_align);
		ImGui::Text("Bits Per Sample: %d bits", current_wave->fmt_chunk.bits_per_sample);
		ImGui::Text("Data Size: %d bytes", current_wave->wave_data.data_size);
		ImGui::Text("Buffer Frames: %d frames", state->bufferFrames);
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

	if (state->loadFile && !state->proccessingAudio) { 
		state->proccessingAudio = true;
		thread load_file_thread(load_file, state, (wchar_t*)NULL); 
		load_file_thread.detach();
	}
	make_playlist(state);
	select_playlist(state);
	popup_Error(state);
	popup_Info(state);
	if (state->last_error[0] != '\0') { ImGui::OpenPopup("Error"); }
	if (state->info_msg[0] != '\0') { ImGui::OpenPopup("Info"); }
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
				if (state->current_sound && !state->proccessingAudio) {

					showSoundInfo(state);

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

	if (ImGui::BeginChild("child2", ImVec2(0, 0)) && state->current_sound != NULL && !state->proccessingAudio) {
		load_graphs(state);
	}
	ImGui::EndChild();
	
	ImGui::SetWindowSize(state->io.DisplaySize);
	ImGui::SetWindowPos(ImVec2(0, 0));
	ImGui::End();
}