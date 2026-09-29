/*
 * Discord Taskbar Typing - Vencord proof of concept
 *
 * Detects TYPING_START events in DM channels and sends the current
 * list of typing users to a local C++ overlay over HTTP.
 *
 * The plugin also starts the overlay .exe (see settings) and hands it
 * Discord's pid, so the overlay closes by itself when Discord quits.
 *
 * The HTTP request is done in native.ts (Electron main process),
 * because Discord's CSP blocks fetch() to 127.0.0.1 from the renderer.
 *
 * Local endpoint:
 *   POST http://127.0.0.1:27345/typing
 */

import { definePluginSettings } from "@api/Settings";
import definePlugin, { OptionType, PluginNative } from "@utils/types";
import { FluxDispatcher, UserStore, SelectedChannelStore, ChannelStore } from "@webpack/common";

// The key must match the plugin "name" below.
const Native = VencordNative.pluginHelpers.TaskbarTyping as PluginNative<typeof import("./native")>;

const settings = definePluginSettings({
    autoLaunch: {
        type: OptionType.BOOLEAN,
        description: "Start the overlay automatically with Discord (it closes when Discord quits)",
        default: true
    },
    overlayPath: {
        type: OptionType.STRING,
        description: "Full path to the overlay .exe. Leave empty to use the default install location.",
        default: ""
    }
});

// Discord re-sends TYPING_START roughly every 8-10 s while someone types.
// Keep a margin so the indicator does not flicker between two events.
const TYPING_TIMEOUT_MS = 12000;
const CLEANUP_INTERVAL_MS = 500;
const MAX_USERS = 3;

type TypingUser = {
    userId: string;
    username: string;
    avatarUrl: string;
    lastTyping: number;
};

const typingUsers = new Map<string, TypingUser>();
let cleanupTimer: ReturnType<typeof setInterval> | undefined;
let overlayWasDown = false;

function avatarUrl(user: any): string {
    if (!user) return "";

    try {
        const raw: string = user.getAvatarURL?.(undefined, 128, false) ?? "";

        if (raw) {
            let url = raw;

            if (url.startsWith("//")) url = "https:" + url;
            else if (url.startsWith("/")) url = "https://discord.com" + url;

            // The overlay decodes PNG (GDI+). WebP / GIF would fail.
            return url.replace(/\.(webp|gif)(\?|$)/, ".png$2");
        }
    } catch { }

    const index = Number((BigInt(user?.id ?? "0") >> 22n) % 6n);
    return `https://cdn.discordapp.com/embed/avatars/${index}.png`;
}

function isDMChannel(channel: any): boolean {
    // Discord channel types: 1 = DM, 3 = GROUP_DM.
    // We intentionally support only 1 for this POC.
    return channel?.type === 1;
}

// Runs while at least one user is typing, then stops by itself.
function ensureCleanupTimer() {
    if (cleanupTimer) return;

    cleanupTimer = setInterval(() => {
        const now = Date.now();
        let removed = false;

        for (const [id, user] of typingUsers) {
            if (now - user.lastTyping > TYPING_TIMEOUT_MS) {
                console.log("[TaskbarTyping] Typing stopped:", user.username);
                typingUsers.delete(id);
                removed = true;
            }
        }

        if (removed)
            sendState();

        if (typingUsers.size === 0) {
            clearInterval(cleanupTimer);
            cleanupTimer = undefined;
        }
    }, CLEANUP_INTERVAL_MS);
}

async function sendState() {
    const users = [...typingUsers.values()]
        .sort((a, b) => b.lastTyping - a.lastTyping)
        .slice(0, MAX_USERS);

    const payload = JSON.stringify({
        users,
        selectedChannelId: SelectedChannelStore.getChannelId?.() ?? null
    });

    try {
        const status = await Native.sendState(payload);

        if (status === 200) {
            if (overlayWasDown)
                console.log("[TaskbarTyping] Overlay reachable again");

            overlayWasDown = false;
            return;
        }

        // status 0 = connection refused / timeout (overlay not running)
        if (!overlayWasDown) {
            console.warn("[TaskbarTyping] Overlay unreachable, status:", status);
            overlayWasDown = true;
        }
    } catch (e) {
        if (!overlayWasDown) {
            console.error("[TaskbarTyping] Native call failed:", e);
            overlayWasDown = true;
        }
    }
}

function onTypingStart(event: any) {
    if (!event?.channelId || !event?.userId)
        return;

    const channel = ChannelStore.getChannel(event.channelId);

    if (!isDMChannel(channel))
        return;

    // Ignore ourselves.
    const currentUser = UserStore.getCurrentUser();
    if (currentUser?.id === event.userId)
        return;

    const user = UserStore.getUser(event.userId);
    if (!user)
        return;

    const username = user.username ?? user.globalName ?? "Unknown";

    if (!typingUsers.has(event.userId))
        console.log("[TaskbarTyping] DM typing:", username);

    typingUsers.set(event.userId, {
        userId: event.userId,
        username,
        avatarUrl: avatarUrl(user),
        lastTyping: Date.now()
    });

    sendState();
    ensureCleanupTimer();
}

function onMessageCreate(event: any) {
    // When someone sends the message, remove them immediately.
    const userId = event?.message?.author?.id ?? event?.author?.id;
    if (!userId) return;

    if (typingUsers.delete(userId))
        sendState();
}

export default definePlugin({
    name: "TaskbarTyping",
    description: "Sends DM typing indicators to a local Windows overlay.",
    authors: [{
        name: "DiscordTaskbarTyping",
        id: 0n
    }],

    // The installer ships this plugin already configured: on by default.
    enabledByDefault: true,

    settings,

    async start() {
        console.log("[TaskbarTyping] Plugin started");

        FluxDispatcher.subscribe("TYPING_START", onTypingStart);
        FluxDispatcher.subscribe("MESSAGE_CREATE", onMessageCreate);

        if (settings.store.autoLaunch) {
            const result = await Native.launchOverlay(settings.store.overlayPath.trim());

            if (result === "started" || result === "already-running")
                console.log("[TaskbarTyping] Overlay:", result);
            else
                console.warn("[TaskbarTyping] Could not start overlay:", result,
                    "(check the exe path in the plugin settings)");
        }
    },

    stop() {
        FluxDispatcher.unsubscribe("TYPING_START", onTypingStart);
        FluxDispatcher.unsubscribe("MESSAGE_CREATE", onMessageCreate);

        typingUsers.clear();

        if (cleanupTimer) {
            clearInterval(cleanupTimer);
            cleanupTimer = undefined;
        }

        sendState();

        // Only stops an overlay that this plugin started.
        Native.stopOverlay();

        console.log("[TaskbarTyping] Plugin stopped");
    }
});
