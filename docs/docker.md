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

Open the port both ways the router lets people in: **TCP** for HTTPS and **UDP** for voice, the same number (8443 unless
`POND_PORT` says otherwise).

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
docker run -d --name pond -p 8443:8443 -p 8443:8443/udp -v pond:/data \
  -e POND_ADDRESS=pond.example.org:8443 -e POND_NAME="My Pond" \
  -e POND_DB_HOST=192.0.2.10 -e POND_DB_USER=pond -e POND_DB_PASSWORD=… -e POND_DB_NAME=pond \
  ghcr.io/ezp2p/pond
```

Without `POND_NAME` it waits instead, until `docker exec -it pond pondctl init "My Pond"`.

## What is kept where

- the `pond` volume (`/data`): `pond.yaml`, and under `data/` the Pond's key (`pond.key`: a Pond with a new key is a
  different Pond, so keep it), its TLS certificate, and uploaded files.
- the `db` volume: MariaDB, so every channel, member and message.

Back up both.
