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
constexpr int broadcast_interval_ms = 50;
constexpr int rescan_interval_ticks = 40;

struct SourceLevel {
	std::string uuid;
	std::string name;
	float magnitude_db = -INFINITY;
	float peak_db = -INFINITY;
	float input_peak_db = -INFINITY;
	int channels = 0;
};

struct AudioMeter {
	std::string uuid;
	std::string name;
	obs_volmeter_t *volmeter = nullptr;
	float magnitude_db = -INFINITY;
	float peak_db = -INFINITY;
	float input_peak_db = -INFINITY;
	int channels = 0;
	std::mutex mutex;

	AudioMeter(std::string meter_uuid, std::string meter_name, obs_source_t *source)
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
		return {uuid, name, magnitude_db, peak_db, input_peak_db, channels};
	}

	static void on_levels_updated(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
				      const float peak[MAX_AUDIO_CHANNELS], const float input_peak[MAX_AUDIO_CHANNELS])
	{
		auto *meter = static_cast<AudioMeter *>(param);
		const int channel_count = std::max(1, obs_volmeter_get_nr_channels(meter->volmeter));
		float max_magnitude = -INFINITY;
		float max_peak = -INFINITY;
		float max_input_peak = -INFINITY;

		for (int index = 0; index < channel_count && index < MAX_AUDIO_CHANNELS; ++index) {
			max_magnitude = std::max(max_magnitude, magnitude[index]);
			max_peak = std::max(max_peak, peak[index]);
			max_input_peak = std::max(max_input_peak, input_peak[index]);
		}

		std::lock_guard<std::mutex> lock(meter->mutex);
		meter->magnitude_db = max_magnitude;
		meter->peak_db = max_peak;
		meter->input_peak_db = max_input_peak;
		meter->channels = channel_count;
	}
};

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
		     << "\",\"magnitudeDb\":";
		append_db(json, level.magnitude_db);
		json << ",\"peakDb\":";
		append_db(json, level.peak_db);
		json << ",\"inputPeakDb\":";
		append_db(json, level.input_peak_db);
		json << ",\"channels\":" << level.channels << '}';
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
		meters.emplace(uuid, std::make_unique<AudioMeter>(uuid, name, source));
	else {
		std::lock_guard<std::mutex> meter_lock(existing->second->mutex);
		existing->second->name = name;
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
	std::lock_guard<std::mutex> lock(meters_mutex);
	levels.reserve(meters.size());
	for (auto &entry : meters)
		levels.push_back(entry.second->snapshot());
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

bool audio_meter_service_start(void)
{
	if (running.exchange(true))
		return true;

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

	ws_server_stop();
	obs_log(LOG_INFO, "audio meter WebSocket server stopped");
}
