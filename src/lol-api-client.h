#pragma once

#include <obs-module.h>
#include <windows.h>
#include <curl/curl.h>

/* ----------------------------------------------------------------- */
/* LoL Live Client Data API client                                    */
/* Polls https://127.0.0.1:2999 for player death events              */
/* ----------------------------------------------------------------- */

struct lol_api_client {
	HANDLE thread;
	HANDLE stop_event;

	CRITICAL_SECTION cs;
	bool game_active;
	bool death_detected;
	int api_death_count;

	float poll_interval; /* seconds, default 1.0 */

	/* Thread-internal state (no lock needed) */
	bool was_dead;
	char player_name[128];
	CURL *curl;
};

void lol_api_client_init(struct lol_api_client *client);
void lol_api_client_start(struct lol_api_client *client);
void lol_api_client_stop(struct lol_api_client *client);
void lol_api_client_free(struct lol_api_client *client);

/* Call from video_tick (reads and clears flags under lock) */
bool lol_api_client_is_game_active(struct lol_api_client *client);
bool lol_api_client_consume_death(struct lol_api_client *client,
				  int *out_deaths);
