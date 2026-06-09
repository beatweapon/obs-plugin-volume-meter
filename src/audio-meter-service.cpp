#include "audio-meter-service.h"

#include "ws-server.h"

#include <obs-audio-controls.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr unsigned short websocket_port = 4457;
constexpr int broadcast_interval_ms = 16;
constexpr int rescan_interval_ticks = 40;

struct ChannelLevel {
	float magnitude_db = -INFINITY;
	float peak_db = -INFINITY;
	float input_peak_db = -INFINITY;
};

struct SourceLevel {
	std::string uuid;
	std::string name;
	bool muted = false;
	std::vector<ChannelLevel> channels;
};

struct AudioMeter {
	std::string uuid;
	std::string name;
	bool muted = false;
	obs_volmeter_t *volmeter = nullptr;
	std::vector<ChannelLevel> channels;
	std::mutex mutex;

	AudioMeter(std::string meter_uuid, std::string meter_name, bool meter_muted, obs_source_t *source)
		: uuid(std::move(meter_uuid)),
		  name(std::move(meter_name))
	{
		volmeter = obs_volmeter_create(OBS_FADER_LOG);
		if (volmeter) {
			obs_volmeter_set_peak_meter_type(volmeter, SAMPLE_PEAK_METER);
			obs_volmeter_add_callback(volmeter, on_levels_updated, this);
			obs_volmeter_attach_source(volmeter, source);
		}
	}

	~AudioMeter()
	{
		if (volmeter) {
			obs_volmeter_remove_callback(volmeter, on_levels_updated, this);
			obs_volmeter_destroy(volmeter);
		}
	}

	SourceLevel snapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return {uuid, name, muted, channels};
	}

	static void on_levels_updated(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
				      const float peak[MAX_AUDIO_CHANNELS], const float input_peak[MAX_AUDIO_CHANNELS])
	{
		auto *meter = static_cast<AudioMeter *>(param);
		const int channel_count = std::max(1, obs_volmeter_get_nr_channels(meter->volmeter));

		std::vector<ChannelLevel> channels;
		channels.reserve(channel_count);
		for (int index = 0; index < channel_count && index < MAX_AUDIO_CHANNELS; ++index) {
			channels.push_back({magnitude[index], peak[index], input_peak[index]});
		}

		std::lock_guard<std::mutex> lock(meter->mutex);
		meter->channels = std::move(channels);
	}
};

struct OutputMeter {
	std::string name = "Master";
	std::vector<ChannelLevel> channels;
	std::mutex mutex;
};

static OutputMeter output_meter;

std::atomic<bool> running(false);
std::thread worker_thread;
std::mutex meters_mutex;
std::map<std::string, std::unique_ptr<AudioMeter>> meters;

std::string json_escape(const std::string &value)
{
	std::ostringstream escaped;
	for (const char character : value) {
		switch (character) {
		case '"':
			escaped << "\\\"";
			break;
		case '\\':
			escaped << "\\\\";
			break;
		case '\b':
			escaped << "\\b";
			break;
		case '\f':
			escaped << "\\f";
			break;
		case '\n':
			escaped << "\\n";
			break;
		case '\r':
			escaped << "\\r";
			break;
		case '\t':
			escaped << "\\t";
			break;
		default:
			if (static_cast<unsigned char>(character) < 0x20) {
				char buffer[7] = {};
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(character));
				escaped << buffer;
			} else {
				escaped << character;
			}
			break;
		}
	}
	return escaped.str();
}

void append_db(std::ostringstream &json, float value)
{
	if (std::isfinite(value))
		json << value;
	else
		json << "null";
}

std::string make_payload(const std::vector<SourceLevel> &levels)
{
	std::ostringstream json;
	json << "{\"type\":\"audio-levels\",\"sources\":[";
	for (size_t index = 0; index < levels.size(); ++index) {
		const auto &level = levels[index];
		if (index > 0)
			json << ',';
		json << "{\"uuid\":\"" << json_escape(level.uuid) << "\",\"name\":\"" << json_escape(level.name)
		     << "\",\"muted\":" << (level.muted ? "true" : "false") << ",\"channels\":[";
		for (size_t ch = 0; ch < level.channels.size(); ++ch) {
			const auto &channel = level.channels[ch];
			if (ch > 0)
				json << ',';
			json << "{\"magnitude\":";
			append_db(json, channel.magnitude_db);
			json << ",\"peak\":";
			append_db(json, channel.peak_db);
			json << ",\"inputPeak\":";
			append_db(json, channel.input_peak_db);
			json << '}';
		}
		json << "]}";
	}
	json << "]}";
	return json.str();
}

bool enum_audio_sources(void *param, obs_source_t *source)
{
	auto *seen = static_cast<std::vector<std::string> *>(param);
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_AUDIO) == 0)
		return true;

	const char *uuid_value = obs_source_get_uuid(source);
	const char *name_value = obs_source_get_name(source);
	if (!uuid_value || !name_value)
		return true;

	std::string uuid(uuid_value);
	std::string name(name_value);
	seen->push_back(uuid);

	std::lock_guard<std::mutex> lock(meters_mutex);
	auto existing = meters.find(uuid);
	if (existing == meters.end())
		meters.emplace(uuid, std::make_unique<AudioMeter>(uuid, name, obs_source_muted(source), source));
	else {
		std::lock_guard<std::mutex> meter_lock(existing->second->mutex);
		existing->second->name = name;
		existing->second->muted = obs_source_muted(source);
	}

	return true;
}

void rescan_sources()
{
	std::vector<std::string> seen;
	obs_enum_sources(enum_audio_sources, &seen);

	std::lock_guard<std::mutex> lock(meters_mutex);
	for (auto it = meters.begin(); it != meters.end();) {
		if (std::find(seen.begin(), seen.end(), it->first) == seen.end())
			it = meters.erase(it);
		else
			++it;
	}
}

std::vector<SourceLevel> snapshot_levels()
{
	std::vector<SourceLevel> levels;

	{
		std::lock_guard<std::mutex> lock(meters_mutex);
		levels.reserve(meters.size());
		for (auto &entry : meters)
			levels.push_back(entry.second->snapshot());
	}
	{
		std::lock_guard<std::mutex> lock(output_meter.mutex);

		if (!output_meter.channels.empty()) {
			SourceLevel master;
			master.uuid = "__master__";
			master.name = output_meter.name;
			master.muted = false;
			master.channels = output_meter.channels;

			levels.push_back(std::move(master));
		}
	}

	return levels;
}

void worker_loop()
{
	int tick = rescan_interval_ticks;
	while (running.load()) {
		if (++tick >= rescan_interval_ticks) {
			rescan_sources();
			tick = 0;
		}

		const auto payload = make_payload(snapshot_levels());
		ws_server_broadcast_text(payload.c_str());
		std::this_thread::sleep_for(std::chrono::milliseconds(broadcast_interval_ms));
	}
}
}



static void on_master_audio(void *param, size_t mix_idx, struct audio_data *data)
{
	if (!data)
		return;

	const uint32_t frames = data->frames;
	if (frames == 0)
		return;

	std::vector<ChannelLevel> channels;

	for (int channel = 0; channel < MAX_AUDIO_CHANNELS; ++channel) {
		float *samples = reinterpret_cast<float *>(data->data[channel]);

		if (!samples)
			break;

		float peak = 0.0f;
		float sum = 0.0f;

		for (uint32_t i = 0; i < frames; ++i) {
			const float sample = samples[i];

			peak = std::max(peak, std::abs(sample));
			sum += sample * sample;
		}

		const float magnitude = std::sqrt(sum / static_cast<float>(frames));

		ChannelLevel level;

		level.magnitude_db = magnitude > 0.0f ? 20.0f * std::log10(magnitude) : -INFINITY;

		level.peak_db = peak > 0.0f ? 20.0f * std::log10(peak) : -INFINITY;

		// MasterにはinputPeakの概念がないのでpeakと同じで良い
		level.input_peak_db = level.peak_db;

		channels.push_back(level);
	}

	{
		std::lock_guard<std::mutex> lock(output_meter.mutex);
		output_meter.channels = std::move(channels);
	}
}


bool audio_meter_service_start(void)
{
	if (running.exchange(true))
		return true;

	audio_convert_info conversion{};
	conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;

	obs_add_raw_audio_callback(0, &conversion, on_master_audio, nullptr);

	if (!ws_server_start(websocket_port)) {
		running.store(false);
		return false;
	}

	worker_thread = std::thread(worker_loop);
	obs_log(LOG_INFO, "audio meter WebSocket server started on ws://127.0.0.1:%u", websocket_port);


	return true;
}

void audio_meter_service_stop(void)
{
	if (!running.exchange(false))
		return;

	if (worker_thread.joinable())
		worker_thread.join();

	{
		std::lock_guard<std::mutex> lock(meters_mutex);
		meters.clear();
	}

	obs_remove_raw_audio_callback(0, on_master_audio, nullptr);

	ws_server_stop();
	obs_log(LOG_INFO, "audio meter WebSocket server stopped");
}
