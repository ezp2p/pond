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

## Running one

Put `compose.yaml` in a directory, and beside it a `.env`:

```sh
POND_ADDRESS=pond.example.org:8443   # how people reach this Pond; what invite links say
POND_NAME=My Pond
POND_DB_PASSWORD=a-long-random-password
# POND_IMAGE=ghcr.io/ezp2p/pond:dev  # the newest build of dev instead of the newest release
```

```sh
docker compose up -d
docker compose exec pond pondctl invite create --uses 10 --expires 7d   # prints the pond://… link to give out
```

Open the port both ways the router lets people in: **TCP** for HTTPS and **UDP** for voice, the same number (8443 unless
`POND_PORT` says otherwise).

On its first start the container writes `/data/pond.yaml` from those variables, waits for MariaDB, and runs `pondctl init`
with `POND_NAME`, which makes the Pond's key and its `#general`. After that the variables aren't read again. To change a
setting, copy `pond.yaml` out (`docker compose cp pond:/data/pond.yaml .`), edit it, copy it back, and
`docker compose restart pond`.

Everything else `pondctl` does works the same way, `docker compose exec pond pondctl <command>`:

```sh
docker compose exec pond pondctl channel create random --topic "Anything goes"
docker compose exec pond pondctl info
```

## Without compose

The image alone, against a MariaDB you already run (a database and a user for the Pond made in it):

```sh
docker run -d --name pond -p 8443:8443 -p 8443:8443/udp -v pond:/data \
  -e POND_ADDRESS=pond.example.org:8443 -e POND_NAME="My Pond" \
  -e POND_DB_HOST=192.0.2.10 -e POND_DB_USER=pond -e POND_DB_PASSWORD=… -e POND_DB_NAME=pond \
  ghcr.io/ezp2p/pond
```

Without `POND_NAME` it waits instead, until `docker exec -it pond pondctl init "My Pond"`.

## Behind a reverse proxy

A Pond can sit behind Apache, nginx, Caddy, Traefik or anything else that terminates TLS, with whatever certificate the
proxy has (Let's Encrypt, renewed whenever). Nothing depends on it: members seal what they send to the Pond's own key
and check its signed receipts (the Puddle Protocol, spec/09), so the certificate only carries the bytes. Point the proxy
at `https://<pond container>:8443` (pondd always speaks TLS; the proxy need not check its self-signed certificate) and:

- pass the request path and query **exactly as sent** (Apache: `ProxyPass ... nocanon`), since members sign them;
- pass the `X-Puddle-Signature`, `X-Puddle-Seal` and `X-Puddle-Receipt` headers and the bodies untouched (no
  compression or rewriting of `application/puddle-sealed`);
- allow answers to wait up to 30 seconds (members hold a request for events open for up to 25);
- leave voice out of it: it is UDP, straight to the Pond's voice port (`voice.port` in `pond.yaml`), forwarded on its own.

## What is kept where

- the `pond` volume (`/data`): `pond.yaml`, and under `data/` the Pond's key (`pond.key`: a Pond with a new key is a
  different Pond, so keep it), its TLS certificate, and uploaded files.
- the `db` volume: MariaDB, so every channel, member and message.

Back up both.
