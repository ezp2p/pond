# Pond in Docker

The Pond is published as a container image, `ghcr.io/ezp2p/pond`, for amd64 and arm64 (a Raspberry Pi 4 or 5 runs it).
It needs MariaDB beside it; `compose.yaml` runs the two.

| Tag            | What it is                                                                     |
|----------------|--------------------------------------------------------------------------------|
| `latest`       | the newest release                                                             |
| `0.2.0`, `0.2` | a release, and the newest release of 0.2                                       |
| `dev`          | the newest build of the `dev` branch, replaced on every push to it: for trying |
| `dev-<commit>` | the build of that commit of `dev`                                              |

`.github/workflows/docker.yml` builds them: a pull request builds both architectures and publishes nothing, a push to
`dev` publishes `dev`, and a tag like `0.2.0` publishes the release (`build.yml` makes its GitHub release). A tag with a
`-`, such as `0.2.0-rc.1`, is a prerelease and doesn't move `latest`.

## Quick start

`compose.yaml` and `.env.example` in the repository are all it takes, without the rest of the checkout:

```sh
mkdir pond && cd pond
curl -fsSLO https://raw.githubusercontent.com/ezp2p/pond/main/compose.yaml
curl -fsSL https://raw.githubusercontent.com/ezp2p/pond/main/.env.example -o .env
$EDITOR .env                       # the address, the name, a database password; POND_IMAGE=…:dev for the newest build
docker compose up -d
docker exec pond pondctl invite create --uses 10 --expires 7d   # prints the pond://… link to give out
```

Open port **443** on the router, for **TCP and UDP** (on most routers one rule, "TCP/UDP" or "Both"). A Pond has two
ports with the same number unless you say otherwise:

| | Protocol | Carries | Setting |
|---|---|---|---|
| **chat port** | TCP | joining, channels, messages, files, invites | `POND_CHAT_PORT`, `ports.chat` in `pond.yaml` (443) |
| **voice port** | UDP | voice and video in voice channels | `POND_VOICE_PORT`, `ports.voice` in `pond.yaml` (the chat port's number) |

Without the UDP rule everything works except voice channels.

On its first start the container writes `/data/pond.yaml` from those variables, waits for MariaDB, and runs `pondctl init`
with `POND_NAME`, which makes the Pond's key and its `#general`. After that the variables aren't read again. To change a
setting, copy `pond.yaml` out (`docker cp pond:/data/pond.yaml .`), edit it, copy it back, and
`docker restart pond`.

Everything else `pondctl` does works the same way, `docker exec pond pondctl <command>`:

```sh
docker exec pond pondctl channel create random --topic "Anything goes"
docker exec pond pondctl info
```

## Without compose

The image alone, against a MariaDB you already run (a database and a user for the Pond made in it):

```sh
docker run -d --name pond -p 443:443 -p 443:443/udp -v pond:/data \
  -e POND_ADDRESS=pond.example.org -e POND_NAME="My Pond" \
  -e POND_DB_HOST=192.0.2.10 -e POND_DB_USER=pond -e POND_DB_PASSWORD=… -e POND_DB_NAME=pond \
  ghcr.io/ezp2p/pond
```

Without `POND_NAME` it waits instead, until `docker exec -it pond pondctl init "My Pond"`.

## Behind a reverse proxy

A Pond can sit behind Apache, nginx, Caddy, Traefik or anything else that terminates TLS, with whatever certificate the
proxy has (Let's Encrypt, renewed whenever). Nothing depends on it: members seal what they send to the Pond's own key
and check its signed receipts (the Puddle Protocol, spec/09), so the certificate only carries the bytes. Point the proxy
at the Pond's chat port, `https://<pond container>:<chat port>` (pondd always speaks TLS; the proxy need not check its
self-signed certificate), and:

- pass the request path and query **exactly as sent** (Apache: `ProxyPass ... nocanon`), since members sign them;
- pass the `X-Puddle-Signature`, `X-Puddle-Seal` and `X-Puddle-Receipt` headers and the bodies untouched (no
  compression or rewriting of `application/puddle-sealed`);
- allow answers to wait up to 30 seconds (members hold a request for events open for up to 25);
- leave voice out of it: the proxy carries only TCP, so forward the voice port (UDP) straight to the Pond. UDP 443 is
  usually free beside a proxy on TCP 443, so the voice port can stay 443. Pick another number only if the proxy uses
  UDP 443 itself (Caddy, and nginx with HTTP/3 turned on).

Behind a proxy the chat port is inside the Docker network, so it can be anything; the proxy listens on 443. For example,
with the Pond reached as `pond.example.org` through the proxy and voice on 443:

```sh
docker run -d --name pond --network proxy -p 443:443/udp -v pond:/data \
  -e POND_ADDRESS=pond.example.org -e POND_CHAT_PORT=8443 -e POND_VOICE_PORT=443 … \
  ghcr.io/ezp2p/pond
```

## What is kept where

- the `pond` volume (`/data`): `pond.yaml`, and under `data/` the Pond's key (`pond.key`: a Pond with a new key is a
  different Pond, so keep it), its TLS certificate, and uploaded files.
- the `db` volume: MariaDB, so every channel, member and message.

Back up both.
