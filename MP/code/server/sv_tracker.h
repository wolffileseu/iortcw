/*
 * Return to Castle Wolfenstein Multiplayer GPL Source Code
 * Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.
 *
 * Tracker integration ported from ET: Legacy
 * Copyright (C) 2012-2024 ET:Legacy team <mail@etlegacy.com>
 * Copyright (C) 2012 Konrad Mosoń <mosonkonrad@gmail.com>
 * iortcw port: Wolffiles contributors, 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
/**
 * @file sv_tracker.h
 * @brief Sends server status to a Wolffiles-compatible tracker.
 *
 * Minimal, QVM-independent subset of the ET: Legacy sv_tracker feature:
 * server lifecycle (start/stop/map) and player presence (connect/disconnect/
 * name) plus a periodic "p" heartbeat. Weapon-stats reporting (wsc/ws), which
 * depends on mod-side QVM cooperation, is intentionally omitted.
 */

#ifndef INCLUDE_TRACKER_H
#define INCLUDE_TRACKER_H

/* server.h is included by the .c files that include this header */

void Tracker_Init(void);
void Tracker_Send(char *format, ...);

void Tracker_ServerStart(void);
void Tracker_ServerStop(void);

void Tracker_ClientConnect(client_t *cl);
void Tracker_ClientDisconnect(client_t *cl);
void Tracker_ClientName(client_t *cl);

void Tracker_Map(char *mapname);
void Tracker_MapRestart(void);
void Tracker_MapEnd(void);

void Tracker_Frame(int msec);
void Tracker_catchBotConnect(int clientNum);

#endif // INCLUDE_TRACKER_H
