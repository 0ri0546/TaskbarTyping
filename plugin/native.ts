/*
 * Runs in the Electron MAIN process (not the renderer), so:
 *  - Discord's CSP does not apply (request to 127.0.0.1 is allowed)
 *  - we can start the overlay .exe, and hand it our own pid so it
 *    closes by itself when Discord quits.
 *
 * Module state survives renderer reloads (Ctrl+R), only a full
 * Discord restart resets it.
 */

import { IpcMainInvokeEvent } from "electron";
import { ChildProcess, spawn } from "child_process";
import { existsSync } from "fs";
import { dirname, extname, isAbsolute, join } from "path";
import { request } from "http";

const HOST = "127.0.0.1";
const PORT = 27345;
const PATH = "/typing";

// Where the installer puts the overlay. Must match DefaultDirName in
// installer/TaskbarTyping.iss (used when the plugin setting is empty).
const DEFAULT_EXE = join(
    process.env.LOCALAPPDATA ?? "",
    "Programs",
    "DiscordTaskbarTyping",
    "DiscordTaskbarTyping.exe"
);

let overlayProcess: ChildProcess | null = null;

// Returns the HTTP status code, or 0 if the overlay is unreachable.
export function sendState(_: IpcMainInvokeEvent, payload: string): Promise<number> {
    return new Promise(resolve => {
        const req = request(
            {
                host: HOST,
                port: PORT,
                path: PATH,
                method: "POST",
                timeout: 2000,
                headers: {
                    "Content-Type": "application/json",
                    "Content-Length": Buffer.byteLength(payload)
                }
            },
            res => {
                res.resume();
                resolve(res.statusCode ?? 0);
            }
        );

        req.on("timeout", () => {
            req.destroy();
            resolve(0);
        });

        req.on("error", () => resolve(0));

        req.end(payload);
    });
}

// Starts the overlay. Returns "started", "already-running", or an error code.
export function launchOverlay(_: IpcMainInvokeEvent, exePath: string): string {
    try {
        if (overlayProcess && overlayProcess.exitCode === null && !overlayProcess.killed)
            return "already-running";

        const target = exePath || (process.env.LOCALAPPDATA ? DEFAULT_EXE : "");

        if (!target)
            return "no-path";

        if (!isAbsolute(target) || extname(target).toLowerCase() !== ".exe")
            return "invalid-path";

        if (!existsSync(target))
            return "not-found";

        // --pid: the overlay watches this process and quits when it ends.
        const child = spawn(target, ["--pid", String(process.pid)], {
            cwd: dirname(target),
            detached: true,
            stdio: "ignore",
            windowsHide: true
        });

        child.on("exit", () => {
            if (overlayProcess === child)
                overlayProcess = null;
        });

        child.on("error", () => {
            if (overlayProcess === child)
                overlayProcess = null;
        });

        child.unref();

        overlayProcess = child;

        return "started";
    } catch {
        return "error";
    }
}

// Stops the overlay, only if this module started it.
export function stopOverlay(_: IpcMainInvokeEvent): boolean {
    if (!overlayProcess)
        return false;

    try {
        overlayProcess.kill();
    } catch { }

    overlayProcess = null;

    return true;
}
