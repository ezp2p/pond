# pond

Community servers for Puddle: someone runs a Pond, makes text channels, and gives out invite links; people join from their
Puddle. The design is on the [wiki](https://www.ezp2p.org/wiki/pond/design/) and the routes in [api/pond.yaml](api/pond.yaml).

What a Pond says to Puddles follows the Puddle Protocol (`ezp2p/protocol`); [the wiki](https://www.ezp2p.org/wiki/pond/design/protocol/) says
which parts Pond implements and what the protocol asks of it next.

- `pondd` is the service. It has no UI and serves HTTPS on its chat port (TCP) and voice on its voice port (UDP), both 443 unless set.
- `pondctl` is the admin's tool. It runs on the same machine and uses the same config.

## Docker

The quickest way to run one: `ghcr.io/ezp2p/pond` (amd64 and arm64), with MariaDB beside it. Take
[compose.yaml](compose.yaml) and [.env.example](.env.example) (as `.env`, filled in), then:

```sh
docker compose up -d
docker exec pond pondctl invite create     # the pond://… link to give out
```

`:latest` is the newest release and `:dev` the newest build of `dev`. [The wiki](https://www.ezp2p.org/wiki/pond/docker/) says more.

## Build

It is C++20, built on [Drogon](https://github.com/drogonframework/drogon), which CMake fetches and builds the first time
(a few minutes). It needs MariaDB (the server, and its C client), OpenSSL 3 or later, hiredis, jsoncpp and yaml-cpp. It
shares one library with Puddle and Levee, libpuddle-proto (the protocol's bytes), from the submodule `third_party/protocol`:
clone with `--recurse-submodules`, or run `git submodule update --init`.

```sh
brew install mariadb openssl hiredis jsoncpp yaml-cpp
# or: apt install mariadb-server libmariadb-dev libssl-dev libhiredis-dev libjsoncpp-dev libyaml-cpp-dev uuid-dev zlib1g-dev
scripts/build.sh                           # builds and runs the tests (each test starts a throwaway MariaDB and pondd)
```

The end-to-end test also needs a built Puddle to join the Pond with: it looks for `../puddle/build/puddled` (set
`-DPUDDLED=` for another), and is skipped without one.

## Run

```sh
mariadb -e "CREATE DATABASE pond; CREATE USER pond@localhost IDENTIFIED BY 'secret'; GRANT ALL ON pond.* TO pond@localhost"
cp pond.yaml.example pond.yaml            # and edit it
build/pondctl init "My Pond"              # makes the key, the tables and #general
build/pondctl invite create --uses 10 --expires 7d
build/pondd
```

Give out the `pond://…` link that `invite create` prints. Someone joins by pasting it into Puddle (the + at the bottom of the
server rail).

`scripts/dev.sh` starts a throwaway MariaDB and a Pond with an invite, for trying things out.

## Docs, decisions and what's next

The design documents that used to be in `docs/` are on the website wiki ([Pond design](https://www.ezp2p.org/wiki/pond/design/), source in
`ezp2p/website` under `src/wiki/pond/`). A comment or spec that names `docs/design.md`, `docs/dms.md`, `docs/voice.md`
or `docs/protocol.md` means the wiki page of that name. When a change alters the design, update the wiki page in a
website pull request alongside. The decisions made along the way are under [Decisions](https://www.ezp2p.org/wiki/decisions/), and the work
still to do is on the [project board](https://github.com/orgs/ezp2p/projects/1).
