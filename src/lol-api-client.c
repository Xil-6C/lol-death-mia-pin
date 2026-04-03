#include "lol-api-client.h"
#include <jansson.h>
#include <string.h>
#include <process.h>

#define API_BASE "https://127.0.0.1:2999"
#define API_PLAYER_NAME API_BASE "/liveclientdata/activeplayername"
#define API_PLAYER_LIST API_BASE "/liveclientdata/playerlist"

/* ----------------------------------------------------------------- */
/* curl write callback -- appends to a dynamic buffer                 */
/* ----------------------------------------------------------------- */

struct curl_buffer {
	char *data;
	size_t size;
};

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
			    void *userp)
{
	size_t total = size * nmemb;
	struct curl_buffer *buf = userp;

	char *tmp = brealloc(buf->data, buf->size + total + 1);
	if (!tmp)
		return 0;

	buf->data = tmp;
	memcpy(buf->data + buf->size, contents, total);
	buf->size += total;
	buf->data[buf->size] = '\0';
	return total;
}

/* ----------------------------------------------------------------- */
/* HTTP GET helper                                                    */
/* ----------------------------------------------------------------- */

static bool api_get(CURL *curl, const char *url, struct curl_buffer *buf)
{
	buf->data = NULL;
	buf->size = 0;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		bfree(buf->data);
		buf->data = NULL;
		buf->size = 0;
		return false;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code != 200) {
		bfree(buf->data);
		buf->data = NULL;
		buf->size = 0;
		return false;
	}

	return true;
}

/* ----------------------------------------------------------------- */
/* Parse activeplayername response (JSON string with quotes)          */
/* ----------------------------------------------------------------- */

static bool parse_player_name(const char *json_str, char *out, size_t out_size)
{
	json_error_t err;
	json_t *root = json_loads(json_str, JSON_DECODE_ANY, &err);
	if (!root)
		return false;

	if (!json_is_string(root)) {
		json_decref(root);
		return false;
	}

	const char *name = json_string_value(root);
	if (!name || !*name) {
		json_decref(root);
		return false;
	}

	snprintf(out, out_size, "%s", name);
	json_decref(root);
	return true;
}

/* ----------------------------------------------------------------- */
/* Parse playerlist and find active player's death state              */
/* ----------------------------------------------------------------- */

static bool parse_player_death(const char *json_str, const char *player_name,
			       bool *out_is_dead, int *out_deaths)
{
	json_error_t err;
	json_t *root = json_loads(json_str, 0, &err);
	if (!root || !json_is_array(root)) {
		json_decref(root);
		return false;
	}

	size_t index;
	json_t *player;
	json_array_foreach(root, index, player)
	{
		/* Match by riotId first, then summonerName */
		json_t *jid = json_object_get(player, "riotId");
		json_t *jsn = json_object_get(player, "summonerName");

		const char *rid = json_is_string(jid)
					  ? json_string_value(jid)
					  : NULL;
		const char *sn = json_is_string(jsn)
					 ? json_string_value(jsn)
					 : NULL;

		bool match = false;
		if (rid && strcmp(rid, player_name) == 0)
			match = true;
		else if (sn && strcmp(sn, player_name) == 0)
			match = true;

		if (!match)
			continue;

		json_t *jdead = json_object_get(player, "isDead");
		if (jdead && json_is_boolean(jdead))
			*out_is_dead = json_is_true(jdead);
		else
			*out_is_dead = false;

		json_t *scores = json_object_get(player, "scores");
		if (scores) {
			json_t *jdeaths =
				json_object_get(scores, "deaths");
			if (jdeaths && json_is_integer(jdeaths))
				*out_deaths = (int)json_integer_value(jdeaths);
		}

		json_decref(root);
		return true;
	}

	json_decref(root);
	return false;
}

/* ----------------------------------------------------------------- */
/* Polling thread                                                     */
/* ----------------------------------------------------------------- */

static unsigned __stdcall poll_thread_func(void *param)
{
	struct lol_api_client *c = param;

	c->curl = curl_easy_init();
	if (!c->curl) {
		blog(LOG_WARNING, "[lol-api] curl_easy_init failed");
		return 0;
	}

	curl_easy_setopt(c->curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(c->curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(c->curl, CURLOPT_TIMEOUT, 2L);
	curl_easy_setopt(c->curl, CURLOPT_CONNECTTIMEOUT, 2L);

	DWORD poll_ms = (DWORD)(c->poll_interval * 1000.0f);
	if (poll_ms < 100)
		poll_ms = 100;

	for (;;) {
		DWORD wait = WaitForSingleObject(c->stop_event, poll_ms);
		if (wait == WAIT_OBJECT_0)
			break; /* stop signaled */

		struct curl_buffer buf;

		/* 1. Get active player name */
		if (!api_get(c->curl, API_PLAYER_NAME, &buf)) {
			EnterCriticalSection(&c->cs);
			c->game_active = false;
			LeaveCriticalSection(&c->cs);
			c->was_dead = false;
			c->player_name[0] = '\0';
			continue;
		}

		if (!parse_player_name(buf.data, c->player_name,
				       sizeof(c->player_name))) {
			bfree(buf.data);
			EnterCriticalSection(&c->cs);
			c->game_active = false;
			LeaveCriticalSection(&c->cs);
			continue;
		}
		bfree(buf.data);

		/* 2. Get player list */
		if (!api_get(c->curl, API_PLAYER_LIST, &buf)) {
			EnterCriticalSection(&c->cs);
			c->game_active = false;
			LeaveCriticalSection(&c->cs);
			continue;
		}

		bool is_dead = false;
		int deaths = 0;
		bool found = parse_player_death(buf.data, c->player_name,
						&is_dead, &deaths);
		bfree(buf.data);

		if (!found) {
			EnterCriticalSection(&c->cs);
			c->game_active = false;
			LeaveCriticalSection(&c->cs);
			continue;
		}

		/* 3. Edge detection: was_dead=false -> is_dead=true */
		bool new_death = !c->was_dead && is_dead;
		c->was_dead = is_dead;

		EnterCriticalSection(&c->cs);
		c->game_active = true;
		if (new_death)
			c->death_detected = true;
		c->api_death_count = deaths;
		LeaveCriticalSection(&c->cs);
	}

	curl_easy_cleanup(c->curl);
	c->curl = NULL;
	return 0;
}

/* ----------------------------------------------------------------- */
/* Public API                                                         */
/* ----------------------------------------------------------------- */

void lol_api_client_init(struct lol_api_client *client)
{
	memset(client, 0, sizeof(*client));
	client->poll_interval = 1.0f;
}

void lol_api_client_start(struct lol_api_client *client)
{
	InitializeCriticalSection(&client->cs);

	client->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!client->stop_event) {
		DeleteCriticalSection(&client->cs);
		return;
	}

	client->thread = (HANDLE)_beginthreadex(
		NULL, 0, poll_thread_func, client, 0, NULL);
	if (!client->thread) {
		CloseHandle(client->stop_event);
		client->stop_event = NULL;
		DeleteCriticalSection(&client->cs);
	}
}

void lol_api_client_stop(struct lol_api_client *client)
{
	if (!client->stop_event)
		return;

	SetEvent(client->stop_event);
	WaitForSingleObject(client->thread, INFINITE);
}

void lol_api_client_free(struct lol_api_client *client)
{
	if (client->thread) {
		CloseHandle(client->thread);
		client->thread = NULL;
	}
	if (client->stop_event) {
		CloseHandle(client->stop_event);
		client->stop_event = NULL;
	}
	DeleteCriticalSection(&client->cs);
}

bool lol_api_client_is_game_active(struct lol_api_client *client)
{
	EnterCriticalSection(&client->cs);
	bool active = client->game_active;
	LeaveCriticalSection(&client->cs);
	return active;
}

bool lol_api_client_consume_death(struct lol_api_client *client,
				  int *out_deaths)
{
	EnterCriticalSection(&client->cs);
	bool detected = client->death_detected;
	client->death_detected = false;
	if (out_deaths)
		*out_deaths = client->api_death_count;
	LeaveCriticalSection(&client->cs);
	return detected;
}
