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
 * @file sv_tracker.c
 * @brief Sends server status to a Wolffiles-compatible tracker (iortcw port).
 *
 * Wire protocol (connectionless UDP OOB, i.e. \xff\xff\xff\xff + text):
 *   start | stop
 *   map <name> | maprestart | mapend
 *   connect <slot> <guid> <name> | disconnect <slot> | name <slot> <guid> <name>
 *   p   (heartbeat, every `waittime` seconds)
 *
 * Ported from ET: Legacy's sv_tracker.c. iortcw-specific adaptations:
 *   - NET_OutOfBandPrint() takes netadr_t BY VALUE here (no &), per the
 *     RTCW-MP / ioquake3 calling convention.
 *   - ET: Legacy gated everything behind (sv_advert & SVA_TRACKER); iortcw
 *     has no sv_advert, so the gate is simply "no resolved endpoints = no-op".
 *   - No FEATURE_TRACKER build flag: compiled unconditionally, gated at
 *     runtime via the sv_tracker cvar.
 *   - Weapon-stats reporting (wsc/ws/statsall) removed: it depends on mod-side
 *     QVM cooperation that differs across RtCW mods. Deferred to a later step.
 */

#include "server.h"
#include "sv_tracker.h"

/*
 * Default tracker endpoint. Empty string = feature OFF by default, which keeps
 * this opt-in and suitable for an upstream iortcw pull request (no unsolicited
 * phone-home). Operators enable it by setting the cvar, e.g. in server.cfg:
 *
 *     set sv_tracker "tracker.wolffiles.eu:4444"
 *
 * Fork builds that always want a fixed default can change this one line, e.g.
 * #define TRACKER_DEFAULT_ENDPOINT "tracker.wolffiles.eu:4444"
 */
#define TRACKER_DEFAULT_ENDPOINT ""

/**
 * @def MAX_TRACKERS
 * @brief Maximum number of tracker endpoints parsed from the sv_tracker cvar.
 *
 * The cvar value is bounded by MAX_CVAR_VALUE_STRING by the cvar system, so
 * fitting MAX_TRACKERS entries only works with reasonably short hosts/IPs.
 */
#define MAX_TRACKERS 8

static long t;             // timestamp of last heartbeat
static int  waittime = 15; // heartbeat interval in seconds

static qboolean maprunning = qfalse;

enum
{
	TR_BOT_NONE,
	TR_BOT_CONNECT
};
static int catchBot    = TR_BOT_NONE;
static int catchBotNum = 0;

static netadr_t trackerAddrs[MAX_TRACKERS];
static int      numTrackerAddrs = 0;
static cvar_t   *sv_tracker_cvar = NULL;

static char *Tracker_getGUID(client_t *cl);

/**
 * @brief Resolve a single address and add it to the tracker list
 * @param[in] addr_str Address string (already trimmed, non-empty)
 */
static void Tracker_AddAddress(const char *addr_str)
{
	if (numTrackerAddrs >= MAX_TRACKERS)
	{
		Com_Printf("Tracker: max trackers (%i) reached, ignoring: %s\n", MAX_TRACKERS, addr_str);
		return;
	}

	Com_Printf("Tracker: resolving %s\n", addr_str);
	if (!NET_StringToAdr(addr_str, &trackerAddrs[numTrackerAddrs], NA_IP))
	{
		Com_Printf("Tracker: couldn't resolve address: %s\n", addr_str);
		return;
	}

	Com_Printf("Tracker: %s resolved to %i.%i.%i.%i:%i\n", addr_str,
	           trackerAddrs[numTrackerAddrs].ip[0],
	           trackerAddrs[numTrackerAddrs].ip[1],
	           trackerAddrs[numTrackerAddrs].ip[2],
	           trackerAddrs[numTrackerAddrs].ip[3],
	           BigShort(trackerAddrs[numTrackerAddrs].port));
	numTrackerAddrs++;
}

/**
 * @brief Parse a whitespace-separated list of endpoints and resolve each
 * @param[in] list Raw value from the sv_tracker cvar
 *
 * Example: "tracker1.example.com:4444 tracker2.example.com:4444"
 */
static void Tracker_ParseAddressList(const char *list)
{
	char buf[MAX_CVAR_VALUE_STRING];
	char *p;
	char *token;

	if (!list || !*list)
	{
		return;
	}

	Q_strncpyz(buf, list, sizeof(buf));
	p = buf;

	while (1)
	{
		token = COM_ParseExt(&p, qfalse);
		if (!token[0])
		{
			break;
		}
		Tracker_AddAddress(token);
	}
}

/**
 * @brief Send a formatted message to every resolved tracker endpoint
 * @param[in] format printf-style format string
 */
void Tracker_Send(char *format, ...)
{
	va_list argptr;
	char    msg[MAX_MSGLEN];
	int     i;

	if (numTrackerAddrs == 0)
	{
		return;
	}

	va_start(argptr, format);
	Q_vsnprintf(msg, sizeof(msg), format, argptr);
	va_end(argptr);

	for (i = 0; i < numTrackerAddrs; i++)
	{
		// iortcw: netadr_t passed BY VALUE (no &)
		NET_OutOfBandPrint(NS_SERVER, trackerAddrs[i], "%s", msg);
	}
}

/**
 * @brief Initialize / re-initialize tracker support from the sv_tracker cvar
 */
void Tracker_Init(void)
{
	sv_tracker_cvar = Cvar_Get("sv_tracker", TRACKER_DEFAULT_ENDPOINT, CVAR_ARCHIVE);
	sv_tracker_cvar->modified = qfalse;

	t               = time(0);
	numTrackerAddrs = 0;

	Tracker_ParseAddressList(sv_tracker_cvar->string);

	if (numTrackerAddrs > 0)
	{
		Com_Printf("Tracker: enabled (%i endpoint(s)).\n", numTrackerAddrs);
	}
}

/**
 * @brief Send info about server startup
 */
void Tracker_ServerStart(void)
{
	Tracker_Send("start");
}

/**
 * @brief Send info about server shutdown
 */
void Tracker_ServerStop(void)
{
	Tracker_Send("stop");
}

/**
 * @brief Send info about a newly connected client
 * @param[in] cl Client
 */
void Tracker_ClientConnect(client_t *cl)
{
	Tracker_Send("connect %i %s %s", (int)(cl - svs.clients), Tracker_getGUID(cl), cl->name);
}

/**
 * @brief Send info when a client disconnects
 * @param[in] cl Client
 */
void Tracker_ClientDisconnect(client_t *cl)
{
	Tracker_Send("disconnect %i", (int)(cl - svs.clients));
}

/**
 * @brief Send info when a player changes name
 * @param[in] cl Client
 */
void Tracker_ClientName(client_t *cl)
{
	if (!*cl->name)
	{
		return;
	}

	Tracker_Send("name %i %s %s", (int)(cl - svs.clients), Tracker_getGUID(cl), Info_ValueForKey(cl->userinfo, "name"));
}

/**
 * @brief Send info when the map changes
 * @param[in] mapname Current map
 */
void Tracker_Map(char *mapname)
{
	Tracker_Send("map %s", mapname);
	maprunning = qtrue;
}

/**
 * @brief Send info when the map restarts (lets the tracker reset its timer)
 */
void Tracker_MapRestart(void)
{
	Tracker_Send("maprestart");
	maprunning = qtrue;
}

/**
 * @brief Send info when the map finishes (intermission)
 */
void Tracker_MapEnd(void)
{
	Tracker_Send("mapend");
	maprunning = qfalse;
}

/**
 * @brief Emit mod-independent Score/Ping/Team for every active client.
 *
 * Reads id-standard fields (PERS_SCORE/PERS_TEAM via SV_GameClientNum,
 * cl->ping) which EVERY RtCW qagame fills, so this works regardless of
 * which mod is loaded. Reuses the existing ws wire format with mask=0
 * (no weapon data) so the tracker parser needs no change.
 *
 *   ws <slot> 1 0 \<ping>\<score>\<team>\0\<name>
 */
void Tracker_WriteScores(void)
{
	int            i;
	client_t      *cl;
	playerState_t *ps;
	int            ping;
	int            score;
	int            team;

	for (i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++)
	{
		if (cl->state != CS_ACTIVE)
		{
			continue;
		}
		ps    = SV_GameClientNum(i);
		score = ps->persistant[PERS_SCORE];
		team  = ps->persistant[PERS_TEAM];
		ping  = cl->ping < 9999 ? cl->ping : 9999;

		// mask=0 => no weapon blocks; clientinfo carries ping/score/team/name
		Tracker_Send("ws %i 1 0 \\%i\\%i\\%i\\0\\%s",
		             i, ping, score, team, cl->name);
	}
}

/**
 * @brief Per-frame tick: hot-reload, deferred bot-connect, heartbeat
 * @param[in] msec Unused (kept for call-site symmetry)
 */
void Tracker_Frame(int msec)
{
	// Hot-reload if the operator changed sv_tracker live (rcon / console)
	if (sv_tracker_cvar && sv_tracker_cvar->modified)
	{
		Com_Printf("Tracker: sv_tracker changed, reinitializing...\n");
		Tracker_Init();
		return;
	}

	// Bots connect before userinfo is fully set; report on the next frame
	if (catchBot == TR_BOT_CONNECT)
	{
		Tracker_ClientConnect(&svs.clients[catchBotNum]);
		catchBot = TR_BOT_NONE;
	}

	if (!(time(0) - waittime > t))
	{
		return;
	}

	Tracker_Send("p"); // heartbeat: signal the tracker the server is alive
	Tracker_WriteScores(); // mod-independent score/ping/team for all clients

	t = time(0);
}

/**
 * @brief Defer a bot connect notification to the next frame
 * @param[in] clientNum Slot of the bot
 */
void Tracker_catchBotConnect(int clientNum)
{
	catchBot    = TR_BOT_CONNECT;
	catchBotNum = clientNum;
}

/**
 * @brief Resolve a stable GUID for stats, preferring cl_guid
 * @param[in] cl Client
 */
static char *Tracker_getGUID(client_t *cl)
{
	char *cl_guid = Info_ValueForKey(cl->userinfo, "cl_guid");

	if (cl_guid[0] && Q_stricmp(cl_guid, "unknown") != 0)
	{
		return cl_guid;
	}

	return "unknown";
}


/**
 * @brief Inspect a game print line and forward obituary kills to the tracker.
 *
 * Called from the G_PRINT system call. The RtCW game module emits one line per
 * kill via G_LogPrintf -> G_Printf:
 *     "Kill: <killer> <victim> <mod>: <name> killed <name> by <MOD_x>\n"
 * We parse only the three leading integers and forward:
 *     kill <killer> <victim> <mod>
 * Everything else is ignored. Cheap: a single prefix compare per print line.
 *
 * @param[in] text The string passed to G_PRINT (no leading timestamp)
 */
void Tracker_GamePrint(const char *text)
{
	int killer, victim, mod;

	if (numTrackerAddrs == 0 || !text)
	{
		return;
	}

	// Weapon stats line from the game (ET ws format, already includes the
	// clientinfo suffix). Forward verbatim, minus trailing newline.
	if (Q_strncmp(text, "ws ", 3) == 0)
	{
		char wsbuf[1024];
		int  len;
		Q_strncpyz(wsbuf, text, sizeof(wsbuf));
		len = (int)strlen(wsbuf);
		while (len > 0 && (wsbuf[len - 1] == '\n' || wsbuf[len - 1] == '\r'))
		{
			wsbuf[--len] = '\0';
		}
		Tracker_Send("%s", wsbuf);
		return;
	}

	if (Q_strncmp(text, "Kill: ", 6) != 0)
	{
		return;
	}

	if (sscanf(text + 6, "%i %i %i", &killer, &victim, &mod) == 3)
	{
		Tracker_Send("kill %i %i %i", killer, victim, mod);
	}
}
