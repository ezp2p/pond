# pond

Community servers for Puddle: someone runs a Pond, makes text channels, and gives out invite links; people join from their
Puddle. The design is in [docs/design.md](docs/design.md) and the routes in [api/pond.yaml](api/pond.yaml).

- `pondd` is the service. It has no UI and serves HTTPS on one port.
- `pondctl` is the admin's tool. It runs on the same machine and uses the same config.

## Build

It is C++20, built on [Drogon](https://github.com/drogonframework/drogon), which CMake fetches and builds the first time
(a few minutes). It needs MariaDB (the server, and its C client), OpenSSL 3 or later, hiredis, jsoncpp and yaml-cpp. It
shares no code with Puddle.

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
