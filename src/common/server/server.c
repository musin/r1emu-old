/**
 *
 *   ██████╗   ██╗ ███████╗ ███╗   ███╗ ██╗   ██╗
 *   ██╔══██╗ ███║ ██╔════╝ ████╗ ████║ ██║   ██║
 *   ██████╔╝ ╚██║ █████╗   ██╔████╔██║ ██║   ██║
 *   ██╔══██╗  ██║ ██╔══╝   ██║╚██╔╝██║ ██║   ██║
 *   ██║  ██║  ██║ ███████╗ ██║ ╚═╝ ██║ ╚██████╔╝
 *   ╚═╝  ╚═╝  ╚═╝ ╚══════╝ ╚═╝     ╚═╝  ╚═════╝
 *
 * @license GNU GENERAL PUBLIC LICENSE - Version 2, June 1991
 *          See LICENSE file for further information
 */


// ---------- Includes ------------
#include "server.h"
#include "router.h"
#include "worker.h"
#include "common/utils/string.h"
#include "common/crypto/crypto.h"


// ------ Structure declaration -------
/**
 * @brief Server is the main component of the network.
 * It accepts packets from a Router that routes the packet following a load balancing algorithm to a Worker.
 */
struct Server
{
    /** The router of the Server */
    Router *router;
    /** 1-* Workers of the Server */
    Worker **workers;

    ServerStartupInfo info;
};

// ------ Static declaration -------


// ------ Extern function implementation -------

Server *
serverNew (
    ServerStartupInfo *info
) {
    Server *self;

    if ((self = calloc(1, sizeof(Server))) == NULL) {
        return NULL;
    }

    if (!serverInit (self, info)) {
        serverDestroy(&self);
        error("Server failed to initialize.");
        return NULL;
    }

    return self;
}

bool
serverInit (
    Server *self,
    ServerStartupInfo *info
) {
    // Make a private copy
    if (!(serverStartupInfoInit (
        &self->info,
        info->serverType,
        &info->routerInfo,
        info->workersInfo,
        info->workersInfoCount,
        info->output)))
    {
        error("Cannot init the ServerStartupInfo");
        return false;
    }

    // Initialize crypto module for decrypting packet
    if (!(cryptoInit())) {
        error ("Cannot initialize crypto module.");
        return false;
    }

    // Initialize router
    if (!(self->router = routerNew (&info->routerInfo))) {
        error("Cannot allocate a new Router.");
        return false;
    }

    // Initialize workers - Start N worker threads.
    if (!(self->workers = malloc (sizeof(Worker *) * info->routerInfo.workersCount))) {
        error("Cannot allocate enough Workers.");
        return false;
    }

    for (uint16_t workerId = 0; workerId < info->routerInfo.workersCount; workerId++)
    {
        // Allocate a new worker
        if (!(self->workers[workerId] = workerNew (&info->workersInfo[workerId]))) {
            error("Cannot allocate a new worker");
            return false;
        }
    }

    return true;
}

bool
serverStartupInfoInit (
    ServerStartupInfo *self,
    ServerType serverType,
    RouterStartupInfo *routerInfo,
    WorkerStartupInfo *workersInfo,
    int workersInfoCount,
    char *output
) {
    // Copy router Info
    memcpy(&self->routerInfo, routerInfo, sizeof(self->routerInfo));

    // Copy Worker info
    self->workersInfo = malloc (sizeof(WorkerStartupInfo) * workersInfoCount);
    for (int i = 0; i < workersInfoCount; i++) {
        memcpy(&self->workersInfo[i], &workersInfo[i], sizeof(WorkerStartupInfo));
    }

    self->serverType = serverType;
    self->workersInfoCount = workersInfoCount;
    self->output = output;

    return true;
}


bool
serverCreateProcess (
    ServerStartupInfo *self,
    char *executableName
) {
    MySQLStartupInfo *sqlInfo = &self->workersInfo[0].sqlInfo;
    RedisStartupInfo *redisInfo = &self->workersInfo[0].redisInfo;
    char *globalServerIp = self->workersInfo[0].globalServerIp;
    int globalServerPort = self->workersInfo[0].globalServerPort;

    #ifdef WIN32
        executableName = zsys_sprintf("%s.exe", executableName);
    #endif

    char *commandLine = zsys_sprintf("%s %d %s %d",
        executableName,
        self->routerInfo.routerId,
        self->routerInfo.ip,
        self->routerInfo.port
    );

    char *lastCommandLine;
    lastCommandLine = zsys_sprintf("%s %d %s %d %s %s %s %s %s %d %d %s",
        commandLine,
        self->routerInfo.workersCount,
        globalServerIp,
        globalServerPort,
        sqlInfo->hostname, sqlInfo->user, sqlInfo->password, sqlInfo->database,
        redisInfo->hostname, redisInfo->port,
        self->serverType,
        self->output
    );

    zstr_free(&commandLine);
    commandLine = lastCommandLine;

    info("CommandLine : %s", commandLine);

    #ifdef WIN32
        STARTUPINFO si = {0};
        PROCESS_INFORMATION pi = {0};
        if (!CreateProcess (executableName, commandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
            error("Cannot launch Zone Server executable : %s.", executableName);
            char *errorReason;
            FormatMessage (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &errorReason, 0, NULL
            );
            error("Error reason : %s", errorReason);
            return false;
        }
    #else
        char **argv = strSplit (commandLine, ' ');
        if (fork () == 0) {
            if (execv (executableName, (char * const *) argv) == -1) {
                error("Cannot launch Zone Server executable : %s.", executableName);
                return false;
            }
        }
        free(argv);
    #endif

    zstr_free(&commandLine);
    return true;
}

bool
serverStart (
    Server *self
) {
    // Start all the Workers
    for (int i = 0; i < self->info.routerInfo.workersCount; i++) {
        if (!(workerStart (self->workers[i]))) {
            error("[routerId=%d][WorkerId=%d] Cannot start the Worker", routerGetId (self->router), i);
            return false;
        }
    }

    // Start the Router
    if (!(routerStart (self->router))) {
        error("[routerId=%d] Cannot start the router.", routerGetId (self->router));
        return false;
    }

    return true;
}

uint16_t
serverGetRouterId (
    Server *self
) {
    return self->info.routerInfo.routerId;
}

void
serverFree (
    Server *self
) {
    for (int i = 0; i < self->info.workersInfoCount; i++) {
        workerDestroy (&self->workers[i]);
    }

    routerDestroy (&self->router);
}

void
serverDestroy(
    Server **_self
) {
    Server *self = *_self;

    if (self) {
        serverFree (self);
        free(self);
    }

    *_self = NULL;
}
