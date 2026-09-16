#include "web_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "audio_player.h"
#include "sd_card.h"
#include "wifi_manager.h"

static const char *TAG = "WEB_SRV";
static httpd_handle_t s_server = NULL;

/* Helper to decode URL-encoded strings in place */
static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t d = 0;
    while (*src && d < dst_len - 1) {
        if (*src == '%' && isxdigit((int)*(src + 1)) && isxdigit((int)*(src + 2))) {
            char hex[3] = { *(src + 1), *(src + 2), '\0' };
            dst[d++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[d++] = ' ';
            src++;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = '\0';
}

/* =========================================================================
   EMBEDDED DASHBOARD HTML/CSS/JS (Dark modern audiophile UI)
   ========================================================================= */
static const char s_dashboard_html[] = 
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>ESP32 Audio Player Dashboard</title>\n"
"<style>\n"
"  :root {\n"
"    --bg: #0b0f19;\n"
"    --card: #131b2e;\n"
"    --card-border: #1e293b;\n"
"    --text: #f8fafc;\n"
"    --text-muted: #94a3b8;\n"
"    --cyan: #38bdf8;\n"
"    --indigo: #818cf8;\n"
"    --emerald: #34d399;\n"
"    --amber: #fbbf24;\n"
"    --rose: #f43f5e;\n"
"  }\n"
"  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }\n"
"  body { background-color: var(--bg); color: var(--text); min-height: 100vh; padding: 20px; display: flex; flex-direction: column; align-items: center; }\n"
"  .container { width: 100%; max-width: 900px; display: flex; flex-direction: column; gap: 20px; }\n"
"  header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 12px; padding: 10px 0; border-bottom: 1px solid var(--card-border); }\n"
"  .brand h1 { font-size: 1.5rem; font-weight: 700; color: var(--cyan); letter-spacing: -0.5px; }\n"
"  .brand span { font-size: 0.85rem; color: var(--text-muted); }\n"
"  .badges { display: flex; gap: 8px; flex-wrap: wrap; }\n"
"  .badge { background: rgba(255,255,255,0.06); border: 1px solid var(--card-border); border-radius: 9999px; padding: 4px 12px; font-size: 0.75rem; display: flex; align-items: center; gap: 6px; }\n"
"  .badge .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--emerald); box-shadow: 0 0 6px var(--emerald); }\n"
"  .badge .dot.red { background: var(--rose); box-shadow: 0 0 6px var(--rose); }\n"
"  .grid { display: grid; grid-template-columns: 1fr; gap: 20px; }\n"
"  @media(min-width: 768px) { .grid { grid-template-columns: 1fr 1fr; } }\n"
"  .card { background: var(--card); border: 1px solid var(--card-border); border-radius: 16px; padding: 20px; box-shadow: 0 8px 30px rgba(0,0,0,0.4); }\n"
"  .card-full { grid-column: 1 / -1; }\n"
"  .card-title { font-size: 1.05rem; font-weight: 600; margin-bottom: 14px; color: var(--text); display: flex; justify-content: space-between; align-items: center; }\n"
"  .player-deck { text-align: center; display: flex; flex-direction: column; gap: 16px; }\n"
"  .track-name { font-size: 1.25rem; font-weight: 700; color: var(--cyan); word-break: break-all; min-height: 1.8rem; }\n"
"  .track-info { display: flex; justify-content: center; gap: 10px; font-size: 0.8rem; color: var(--text-muted); }\n"
"  .progress-bar-wrap { width: 100%; height: 8px; background: rgba(255,255,255,0.1); border-radius: 4px; overflow: hidden; position: relative; }\n"
"  .progress-bar-fill { height: 100%; width: 0%; background: linear-gradient(90deg, var(--cyan), var(--indigo)); transition: width 0.3s; }\n"
"  .progress-time { display: flex; justify-content: space-between; font-size: 0.75rem; color: var(--text-muted); }\n"
"  .controls { display: flex; justify-content: center; gap: 14px; margin-top: 6px; }\n"
"  button { cursor: pointer; border: none; outline: none; border-radius: 8px; font-weight: 600; font-size: 0.85rem; padding: 10px 18px; transition: all 0.2s ease; display: inline-flex; align-items: center; gap: 6px; }\n"
"  .btn-primary { background: var(--cyan); color: #000; box-shadow: 0 0 15px rgba(56,189,248,0.3); }\n"
"  .btn-primary:hover { transform: translateY(-2px); box-shadow: 0 0 20px rgba(56,189,248,0.5); }\n"
"  .btn-secondary { background: rgba(255,255,255,0.08); color: var(--text); border: 1px solid var(--card-border); }\n"
"  .btn-secondary:hover { background: rgba(255,255,255,0.15); }\n"
"  .btn-danger { background: rgba(244,63,94,0.15); color: var(--rose); border: 1px solid rgba(244,63,94,0.3); }\n"
"  .btn-danger:hover { background: var(--rose); color: #fff; }\n"
"  .btn-play-sm { padding: 4px 10px; font-size: 0.75rem; border-radius: 6px; background: rgba(56,189,248,0.15); color: var(--cyan); border: 1px solid rgba(56,189,248,0.3); }\n"
"  .btn-play-sm:hover { background: var(--cyan); color: #000; }\n"
"  .file-list { max-height: 280px; overflow-y: auto; display: flex; flex-direction: column; gap: 6px; }\n"
"  .file-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 12px; background: rgba(255,255,255,0.03); border: 1px solid var(--card-border); border-radius: 8px; font-size: 0.85rem; }\n"
"  .file-item:hover { background: rgba(255,255,255,0.06); }\n"
"  .file-meta { display: flex; align-items: center; gap: 8px; overflow: hidden; }\n"
"  .file-icon { font-size: 1rem; }\n"
"  .file-name { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 280px; }\n"
"  .file-size { font-size: 0.75rem; color: var(--text-muted); margin-left: 6px; }\n"
"  .tone-controls { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; }\n"
"  input[type=number] { background: rgba(255,255,255,0.06); border: 1px solid var(--card-border); color: #fff; padding: 8px 12px; border-radius: 8px; font-size: 0.85rem; width: 100px; }\n"
"  .toast { position: fixed; bottom: 20px; right: 20px; background: #1e293b; border: 1px solid var(--cyan); color: #fff; padding: 12px 20px; border-radius: 10px; font-size: 0.85rem; opacity: 0; pointer-events: none; transition: opacity 0.3s; z-index: 100; box-shadow: 0 10px 25px rgba(0,0,0,0.5); }\n"
"  .toast.show { opacity: 1; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n"
"  <header>\n"
"    <div class=\"brand\">\n"
"      <h1>ESP32 Audio Player</h1>\n"
"      <span>A2DP SBC-XQ Hi-Fi Bluetooth Source</span>\n"
"    </div>\n"
"    <div class=\"badges\">\n"
"      <div class=\"badge\"><div class=\"dot\" id=\"dotWifi\"></div><span id=\"wifiSsid\">rouf.iot</span></div>\n"
"      <div class=\"badge\"><span id=\"wifiIp\">...</span></div>\n"
"      <div class=\"badge\"><span id=\"psramFree\">PSRAM: ...</span></div>\n"
"      <div class=\"badge\"><div class=\"dot\" id=\"dotSd\"></div><span id=\"sdStatus\">SD Card</span></div>\n"
"    </div>\n"
"  </header>\n"
"\n"
"  <div class=\"card card-full player-deck\">\n"
"    <div class=\"track-name\" id=\"trackName\">No Track Loaded</div>\n"
"    <div class=\"track-info\">\n"
"      <span id=\"audioState\">State: STOPPED</span> &bull;\n"
"      <span id=\"audioSource\">Source: NONE</span> &bull;\n"
"      <span id=\"audioFormat\">44.1 kHz / 16-bit / Stereo</span>\n"
"    </div>\n"
"    <div class=\"progress-bar-wrap\">\n"
"      <div class=\"progress-bar-fill\" id=\"progBar\"></div>\n"
"    </div>\n"
"    <div class=\"progress-time\">\n"
"      <span id=\"progPlayed\">0 KB</span>\n"
"      <span id=\"progTotal\">0 KB</span>\n"
"    </div>\n"
"    <div class=\"controls\">\n"
"      <button class=\"btn-primary\" onclick=\"ctrl('play')\">&#9654; Play</button>\n"
"      <button class=\"btn-secondary\" onclick=\"ctrl('pause')\">&#10074;&#10074; Pause</button>\n"
"      <button class=\"btn-danger\" onclick=\"ctrl('stop')\">&#9632; Stop</button>\n"
"      <button class=\"btn-secondary\" onclick=\"reboot()\">&#8634; Reboot</button>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class=\"grid\">\n"
"    <div class=\"card\">\n"
"      <div class=\"card-title\">\n"
"        <span>MicroSD Tracks</span>\n"
"        <button class=\"btn-secondary\" style=\"padding:4px 10px; font-size:0.75rem;\" onclick=\"fetchFiles()\">&#8635; Refresh</button>\n"
"      </div>\n"
"      <div class=\"file-list\" id=\"fileList\">\n"
"        <div style=\"color:var(--text-muted); font-size:0.85rem;\">Loading SD files...</div>\n"
"      </div>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <div class=\"card-title\">Sine Tone Generator</div>\n"
"      <p style=\"font-size:0.8rem; color:var(--text-muted); margin-bottom:14px;\">Synthesize pure mathematical sine waves directly on the DSP core.</p>\n"
"      <div class=\"tone-controls\">\n"
"        <button class=\"btn-secondary\" onclick=\"playTone(440)\">440 Hz (A4)</button>\n"
"        <button class=\"btn-secondary\" onclick=\"playTone(1000)\">1000 Hz</button>\n"
"        <button class=\"btn-secondary\" onclick=\"playTone(100)\">100 Hz</button>\n"
"        <div style=\"display:flex; gap:6px; margin-top:10px; width:100%;\">\n"
"          <input type=\"number\" id=\"customFreq\" value=\"880\" min=\"20\" max=\"20000\">\n"
"          <button class=\"btn-primary\" onclick=\"playCustomTone()\">Play Tone</button>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"<div class=\"toast\" id=\"toast\"></div>\n"
"\n"
"<script>\n"
"function showToast(msg) {\n"
"  const t = document.getElementById('toast');\n"
"  t.innerText = msg;\n"
"  t.classList.add('show');\n"
"  setTimeout(() => t.classList.remove('show'), 3000);\n"
"}\n"
"function formatBytes(bytes) {\n"
"  if (!bytes || bytes === 0) return '0 B';\n"
"  const k = 1024, sizes = ['B', 'KB', 'MB', 'GB'];\n"
"  const i = Math.floor(Math.log(bytes) / Math.log(k));\n"
"  return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];\n"
"}\n"
"function ctrl(action) {\n"
"  fetch('/api/player/' + action, { method: 'POST' })\n"
"    .then(r => r.json())\n"
"    .then(() => { showToast('Action: ' + action.toUpperCase()); updateStatus(); })\n"
"    .catch(e => showToast('Error: ' + e));\n"
"}\n"
"function playFile(path) {\n"
"  showToast('Playing ' + path.split('/').pop() + '...');\n"
"  fetch('/api/player/play_file?path=' + encodeURIComponent(path), { method: 'POST' })\n"
"    .then(r => r.json())\n"
"    .then(res => { updateStatus(); })\n"
"    .catch(e => showToast('Play file error: ' + e));\n"
"}\n"
"function playTone(freq) {\n"
"  showToast('Playing sine tone: ' + freq + ' Hz');\n"
"  fetch('/api/player/tone?freq=' + freq, { method: 'POST' })\n"
"    .then(r => r.json())\n"
"    .then(() => updateStatus());\n"
"}\n"
"function playCustomTone() {\n"
"  const f = document.getElementById('customFreq').value;\n"
"  if (f) playTone(parseFloat(f));\n"
"}\n"
"function reboot() {\n"
"  if (confirm('Reboot ESP32?')) {\n"
"    fetch('/api/system/restart', { method: 'POST' });\n"
"    showToast('Rebooting ESP32... reconnecting in 5s');\n"
"  }\n"
"}\n"
"function updateStatus() {\n"
"  fetch('/api/status')\n"
"    .then(r => r.json())\n"
"    .then(data => {\n"
"      if (data.wifi) {\n"
"        document.getElementById('wifiSsid').innerText = data.wifi.ssid + ' (' + data.wifi.rssi + ' dBm)';\n"
"        document.getElementById('wifiIp').innerText = data.wifi.ip;\n"
"        document.getElementById('dotWifi').className = data.wifi.connected ? 'dot' : 'dot red';\n"
"      }\n"
"      if (data.memory) {\n"
"        document.getElementById('psramFree').innerText = 'PSRAM: ' + formatBytes(data.memory.psram_free);\n"
"      }\n"
"      if (data.sdcard) {\n"
"        document.getElementById('dotSd').className = data.sdcard.mounted ? 'dot' : 'dot red';\n"
"        document.getElementById('sdStatus').innerText = data.sdcard.mounted ? 'SD Mounted' : 'No SD';\n"
"      }\n"
"      if (data.player) {\n"
"        const p = data.player;\n"
"        document.getElementById('audioState').innerText = 'State: ' + p.state;\n"
"        document.getElementById('audioSource').innerText = 'Source: ' + p.source;\n"
"        let name = 'Idle';\n"
"        if (p.source === 'WAV' && p.file) name = p.file.split('/').pop();\n"
"        else if (p.source === 'SINE') name = 'Sine Test Tone';\n"
"        document.getElementById('trackName').innerText = name;\n"
"        document.getElementById('audioFormat').innerText = p.sample_rate + ' Hz / ' + p.bit_depth + '-bit / ' + (p.channels === 2 ? 'Stereo' : 'Mono');\n"
"        const pct = p.total_bytes > 0 ? ((p.played_bytes / p.total_bytes) * 100).toFixed(1) : (p.state === 'PLAYING' ? 100 : 0);\n"
"        document.getElementById('progBar').style.width = pct + '%';\n"
"        document.getElementById('progPlayed').innerText = formatBytes(p.played_bytes);\n"
"        document.getElementById('progTotal').innerText = formatBytes(p.total_bytes);\n"
"      }\n"
"    })\n"
"    .catch(() => {});\n"
"}\n"
"function fetchFiles() {\n"
"  fetch('/api/files')\n"
"    .then(r => r.json())\n"
"    .then(data => {\n"
"      const list = document.getElementById('fileList');\n"
"      list.innerHTML = '';\n"
"      if (!data.files || data.files.length === 0) {\n"
"        list.innerHTML = '<div style=\"color:var(--text-muted);font-size:0.85rem;\">No files found on SD card.</div>';\n"
"        return;\n"
"      }\n"
"      data.files.forEach(f => {\n"
"        const item = document.createElement('div');\n"
"        item.className = 'file-item';\n"
"        const isAudio = /\\.(wav|flac|pcm|mp3)$/i.test(f.name);\n"
"        const icon = f.is_dir ? '&#128193;' : (isAudio ? '&#127925;' : '&#128196;');\n"
"        let playBtn = '';\n"
"        if (isAudio) {\n"
"          playBtn = `<button class=\"btn-play-sm\" onclick=\"playFile('/sdcard/${f.name}')\">&#9654; Play</button>`;\n"
"        }\n"
"        item.innerHTML = `\n"
"          <div class=\"file-meta\">\n"
"            <span class=\"file-icon\">${icon}</span>\n"
"            <span class=\"file-name\" title=\"${f.name}\">${f.name}</span>\n"
"            <span class=\"file-size\">${f.is_dir ? '[DIR]' : formatBytes(f.size)}</span>\n"
"          </div>\n"
"          ${playBtn}\n"
"        `;\n"
"        list.appendChild(item);\n"
"      });\n"
"    })\n"
"    .catch(e => {\n"
"      document.getElementById('fileList').innerHTML = '<div style=\"color:var(--rose);font-size:0.85rem;\">Failed to list SD files.</div>';\n"
"    });\n"
"}\n"
"setInterval(updateStatus, 1500);\n"
"updateStatus();\n"
"fetchFiles();\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* =========================================================================
   HTTP HANDLERS
   ========================================================================= */

/* GET / - Serves Dashboard UI */
static esp_err_t get_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, s_dashboard_html, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/status - JSON state snapshot */
static esp_err_t get_status_handler(httpd_req_t *req)
{
    char ip[20] = {0};
    wifi_manager_get_ip(ip, sizeof(ip));
    int8_t rssi = wifi_manager_get_rssi();
    bool wifi_ok = wifi_manager_is_connected();
    const char *ssid = wifi_manager_get_ssid();

    size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    audio_player_status_t a_status;
    audio_player_get_status(&a_status);

    const char *state_str = "STOPPED";
    if (a_status.state == AUDIO_STATE_PLAYING) state_str = "PLAYING";
    else if (a_status.state == AUDIO_STATE_PAUSED) state_str = "PAUSED";

    const char *source_str = "NONE";
    if (a_status.source == AUDIO_SOURCE_WAV) source_str = "WAV";
    else if (a_status.source == AUDIO_SOURCE_SINE) source_str = "SINE";

    char json[768];
    snprintf(json, sizeof(json),
             "{"
             "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
             "\"memory\":{\"sram_free\":%u,\"psram_free\":%u},"
             "\"player\":{"
               "\"state\":\"%s\","
               "\"source\":\"%s\","
               "\"file\":\"%s\","
               "\"sample_rate\":%lu,"
               "\"channels\":%u,"
               "\"bit_depth\":%u,"
               "\"total_bytes\":%lu,"
               "\"played_bytes\":%lu"
             "},"
             "\"sdcard\":{\"mounted\":%s}"
             "}",
             wifi_ok ? "true" : "false",
             ssid ? ssid : "",
             ip,
             (int)rssi,
             (unsigned int)sram_free,
             (unsigned int)psram_free,
             state_str,
             source_str,
             a_status.current_path,
             (unsigned long)a_status.sample_rate,
             (unsigned int)a_status.channels,
             (unsigned int)a_status.bit_depth,
             (unsigned long)a_status.total_audio_bytes,
             (unsigned long)a_status.played_audio_bytes,
             sd_card_is_mounted() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/files - MicroSD card directory listing */
static esp_err_t get_files_handler(httpd_req_t *req)
{
    if (!sd_card_is_mounted()) {
        /* Auto-mount attempt */
        sd_card_init();
    }

    if (!sd_card_is_mounted()) {
        const char *err_json = "{\"error\":\"SD card not mounted\",\"files\":[]}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, err_json, HTTPD_RESP_USE_STRLEN);
    }

    /* Extract query parameter path if provided */
    char query[256] = {0};
    char target_dir[256] = SD_CARD_MOUNT_POINT;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[256] = {0};
        if (httpd_query_key_value(query, "path", param, sizeof(param)) == ESP_OK) {
            url_decode(target_dir, param, sizeof(target_dir));
        }
    }

    DIR *dir = opendir(target_dir);
    if (!dir) {
        const char *err_json = "{\"error\":\"Unable to open path\",\"files\":[]}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, err_json, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Stream response chunk by chunk to avoid allocating huge buffer in RAM */
    httpd_resp_sendstr_chunk(req, "{\"path\":\"");
    httpd_resp_sendstr_chunk(req, target_dir);
    httpd_resp_sendstr_chunk(req, "\",\"files\":[");

    struct dirent *entry;
    bool first = true;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", target_dir, entry->d_name);

        struct stat st;
        bool is_dir = false;
        long size = 0;
        if (stat(full_path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (long)st.st_size;
        }

        char item_json[384];
        snprintf(item_json, sizeof(item_json),
                 "%s{\"name\":\"%s\",\"size\":%ld,\"is_dir\":%s}",
                 first ? "" : ",",
                 entry->d_name,
                 size,
                 is_dir ? "true" : "false");

        httpd_resp_sendstr_chunk(req, item_json);
        first = false;
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL); /* End chunked transmission */
    return ESP_OK;
}

/* POST /api/player/play */
static esp_err_t post_play_handler(httpd_req_t *req)
{
    audio_player_play();
    const char *resp = "{\"status\":\"ok\",\"action\":\"play\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/player/pause */
static esp_err_t post_pause_handler(httpd_req_t *req)
{
    audio_player_pause();
    const char *resp = "{\"status\":\"ok\",\"action\":\"pause\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/player/stop */
static esp_err_t post_stop_handler(httpd_req_t *req)
{
    audio_player_stop();
    const char *resp = "{\"status\":\"ok\",\"action\":\"stop\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/player/play_file */
static esp_err_t post_play_file_handler(httpd_req_t *req)
{
    char target_path[256] = {0};

    /* Check URL query string first (e.g. ?path=/sdcard/song.wav) */
    char query[384] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[256] = {0};
        if (httpd_query_key_value(query, "path", param, sizeof(param)) == ESP_OK) {
            url_decode(target_path, param, sizeof(target_path));
        }
    }

    /* Fall back to request body if query wasn't present */
    if (target_path[0] == '\0' && req->content_len > 0) {
        char body[256] = {0};
        int ret = httpd_req_recv(req, body, sizeof(body) - 1);
        if (ret > 0) {
            body[ret] = '\0';
            /* Strip quotes or newlines if any */
            char *p = body;
            while (*p == ' ' || *p == '"') p++;
            char *end = p + strlen(p) - 1;
            while (end > p && (*end == ' ' || *end == '"' || *end == '\r' || *end == '\n')) {
                *end-- = '\0';
            }
            strncpy(target_path, p, sizeof(target_path) - 1);
        }
    }

    if (target_path[0] == '\0') {
        const char *err_resp = "{\"status\":\"error\",\"message\":\"Missing path parameter\"}";
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, err_resp, HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "Request to play file: '%s'", target_path);
    esp_err_t err = audio_player_play_file(target_path);
    if (err != ESP_OK) {
        char err_resp[128];
        snprintf(err_resp, sizeof(err_resp), "{\"status\":\"error\",\"code\":%d,\"message\":\"%s\"}", err, esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, err_resp, HTTPD_RESP_USE_STRLEN);
    }

    char ok_resp[300];
    snprintf(ok_resp, sizeof(ok_resp), "{\"status\":\"ok\",\"playing\":\"%s\"}", target_path);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok_resp, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/player/tone */
static esp_err_t post_tone_handler(httpd_req_t *req)
{
    double freq = 440.0;
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[32] = {0};
        if (httpd_query_key_value(query, "freq", param, sizeof(param)) == ESP_OK) {
            freq = atof(param);
        }
    } else if (req->content_len > 0) {
        char body[32] = {0};
        int ret = httpd_req_recv(req, body, sizeof(body) - 1);
        if (ret > 0) {
            freq = atof(body);
        }
    }

    if (freq <= 0.0) freq = 440.0;
    audio_player_set_tone(freq);

    char ok_resp[64];
    snprintf(ok_resp, sizeof(ok_resp), "{\"status\":\"ok\",\"freq\":%.1f}", freq);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok_resp, HTTPD_RESP_USE_STRLEN);
}

/* One-shot deferred reboot task */
static void deferred_reboot_task(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* POST /api/system/restart */
static esp_err_t post_restart_handler(httpd_req_t *req)
{
    const char *resp = "{\"status\":\"rebooting\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(deferred_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* =========================================================================
   SERVER LIFECYCLE
   ========================================================================= */

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d...", config.server_port);
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register URI handlers */
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_index_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = get_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &status_uri);

    httpd_uri_t files_uri = {
        .uri = "/api/files",
        .method = HTTP_GET,
        .handler = get_files_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &files_uri);

    httpd_uri_t play_uri = {
        .uri = "/api/player/play",
        .method = HTTP_POST,
        .handler = post_play_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &play_uri);

    httpd_uri_t pause_uri = {
        .uri = "/api/player/pause",
        .method = HTTP_POST,
        .handler = post_pause_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &pause_uri);

    httpd_uri_t stop_uri = {
        .uri = "/api/player/stop",
        .method = HTTP_POST,
        .handler = post_stop_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &stop_uri);

    httpd_uri_t play_file_uri = {
        .uri = "/api/player/play_file",
        .method = HTTP_POST,
        .handler = post_play_file_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &play_file_uri);

    httpd_uri_t tone_uri = {
        .uri = "/api/player/tone",
        .method = HTTP_POST,
        .handler = post_tone_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &tone_uri);

    httpd_uri_t restart_uri = {
        .uri = "/api/system/restart",
        .method = HTTP_POST,
        .handler = post_restart_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &restart_uri);

    ESP_LOGI(TAG, "Web server started and endpoints registered successfully.");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Stopping HTTP server...");
    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    return ret;
}

bool web_server_is_running(void)
{
    return (s_server != NULL);
}
