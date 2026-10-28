# pond

Community servers for Puddle: someone runs a Pond, makes text channels, and gives out invite links; people join from their
Puddle. The design is in [docs/design.md](docs/design.md) and the routes in [api/pond.yaml](api/pond.yaml).

- `pondd` is the service. It has no UI and serves HTTPS on one port.
- `pondctl` is the admin's tool. It runs on the same machine and uses the same config.

## Build

It needs MariaDB (the server, and its C client) and OpenSSL 3. It also needs a Puddle checkout beside this one, at `../puddle`,
because it shares the Puddle's lower layers.

```sh
brew install mariadb openssl@3 libyaml     # or: apt install mariadb-server libmariadb-dev libssl-dev libyaml-dev
scripts/build.sh                           # builds and runs the tests (each test starts a throwaway MariaDB)
```

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
