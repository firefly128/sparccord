# SPARCcord

A native Motif/CDE Discord client for Solaris 7 SPARC, paired with a headless
Chrome bridge server running in Docker.

```
┌──────────────────────┐         HTTP/1.0         ┌──────────────────────┐
│    SPARCstation 4/5   │  ◄──── polling ────────► │   Docker Host        │
│                       │                          │                      │
│  sparccord (C/Motif)  │  GET /api/messages       │  discord-bridge      │
│  X11 + CDE            │  POST /api/send          │  (Node + Puppeteer)  │
│  Solaris 7            │  GET /api/image           │  Headless Chromium   │
│  32-bit SPARC         │                          │  CDP WebSocket       │
└──────────────────────┘                           └──────────┬───────────┘
                                                              │
                                                              │ WebSocket
                                                              ▼
                                                   ┌──────────────────────┐
                                                   │   Discord Gateway    │
                                                   │   + REST API         │
                                                   └──────────────────────┘
```

## Architecture

**discord-bridge/** — Node.js server running inside Docker with headless Chromium.
Logs into Discord via the web app, intercepts Gateway WebSocket frames through
Chrome DevTools Protocol (CDP), and exposes a simple HTTP/1.0 REST API.

**sparccord/** — Native C89/Motif client that runs on Solaris 7 SPARC under CDE.
Polls the bridge for messages, presence, and typing indicators. Displays them in
a classic 3-pane layout (channels | messages | members).

## Quick Start

### 1. Start the bridge (on your Docker host)

```bash
cd discord-bridge
cp .env.example .env
# Edit .env and add your Discord token
vi .env

docker compose up -d
docker compose logs -f
```

The bridge listens on port 3002. It launches Chromium, injects your token,
navigates to Discord, and begins intercepting Gateway events via CDP.

### 2. Build the client (on Solaris 7)

```bash
cd sparccord
make
```

Requires:
- GCC 4.7+ from tgcware (`/usr/tgcware/gcc47/bin/gcc`)
- Motif runtime (SUNWmfrun — included in SUNWCXall)
- X11 platform (SUNWxwplt)
- CDE base (SUNWdtbas)

### 3. Configure

Create `~/.sparccordrc`:

```
server=10.0.2.2
port=3002
poll_ms=2000
image_colors=16
image_maxwidth=320
```

The default server `10.0.2.2` is the QEMU SLiRP gateway — it reaches the Docker
host from inside the guest.

### 4. Run

```bash
DISPLAY=:0 ./sparccord
```

Or with command-line overrides:

```bash
./sparccord -server 10.0.2.2 -port 3002
```

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Bridge connection status |
| GET | `/api/guilds` | List guilds (servers) |
| GET | `/api/channels?guild=ID` | List channels in a guild |
| GET | `/api/messages?channel=ID&limit=N&after=ID` | Fetch messages |
| POST | `/api/send` | Send message `{"channel":"ID","content":"text"}` |
| GET | `/api/presence?guild=ID` | Member presence/status |
| GET | `/api/typing?channel=ID` | Who is currently typing |
| GET | `/api/image?url=URL&w=320&colors=16` | Proxy + convert image to GIF |
| GET | `/api/avatar?user=ID&size=32` | User avatar as GIF |

All responses are JSON except image endpoints which return `image/gif`.

## Building a Package

On the Solaris 7 system:

```bash
sh build-pkg.sh
# Produces: sparccord-0.1-sparc.pkg
pkgadd -d sparccord-0.1-sparc.pkg
```

Installs to `/opt/sparccord/bin/sparccord`.

## Network Setup

With QEMU SLiRP networking (default in sparc-build-host):

- Guest IP: `10.0.2.15`
- Host gateway: `10.0.2.2`
- Bridge port: `3002` (mapped from Docker)

The guest reaches the bridge at `10.0.2.2:3002`.

## File Structure

```
discord-bridge/
    Dockerfile          Headless Chrome + Node.js image
    docker-compose.yml  Service definition
    package.json        Node dependencies
    server.js           Bridge server (~500 lines)
    .env.example        Token placeholder
    .gitignore

sparccord/
    sparccord.c         Main Motif application
    http.c / http.h     HTTP/1.0 client (BSD sockets)
    json.c / json.h     Recursive descent JSON parser
    gifload.c / gifload.h  GIF decoder → X11 Pixmap
    Makefile            GCC 4.7 + Motif build
    build-pkg.sh        SVR4 package builder
    pkg/                Package metadata
        pkginfo         Package info
        depend          Dependencies
        postinstall     Post-install script
        preremove       Pre-remove script
```

## How It Works

1. **discord-bridge** launches Chromium with Puppeteer, injects the Discord
   token via `localStorage`, and navigates to `discord.com/channels/@me`.

2. A CDP session intercepts all WebSocket frames on the page. Gateway dispatch
   events (MESSAGE_CREATE, PRESENCE_UPDATE, GUILD_CREATE, etc.) are parsed and
   cached in memory.

3. **sparccord** polls the bridge via HTTP/1.0 every 2 seconds for new messages
   and every 10 seconds for presence updates.

4. Images are proxied through the bridge which uses ImageMagick to resize,
   dither (Floyd-Steinberg), and convert to 16-color GIF — suitable for 8-bit
   PseudoColor X11 displays.

5. The GIF decoder on the client side uses the PizzaFool color-cache pattern:
   attempt `XAllocColor`, fall back to nearest-match in the existing colormap
   using Euclidean RGB distance with 8-bit values (avoiding 32-bit overflow on
   SPARC).

## License

MIT
