# ops

Scripts for running this server on its Windows host. The folder layout they
assume is in the table below. None of them contain secrets: passwords are
read from `D:\WoWServer\secrets\`, which only the owner and SYSTEM can read.

| path | what |
|---|---|
| `D:\WoWServer\src` | this repository |
| `D:\WoWServer\build` | the CMake build tree |
| `D:\WoWServer\server` | exes, DLLs and generated `.conf` files |
| `D:\WoWServer\data` | dbc, maps, vmaps, mmaps, Cameras |
| `D:\WoWServer\client` | the 3.3.5a client used for map extraction and testing |
| `D:\WoWServer\web` | what the web server serves: `downloads\patches\…` |
| `D:\WoWServer\mysql` | `my.ini` and the database files (service `MySQL84`) |
| `D:\WoWServer\secrets` | generated passwords; never committed |

| script | when |
|---|---|
| `bootstrap/gen-secrets.py` | once: generates the MySQL passwords |
| `bootstrap/admin-install-toolchain.ps1` | once, elevated: OpenSSL, MySQL, Boost, Visual Studio |
| `import-db.sh` | from Git Bash: loads the database snapshot (`FORCE=1` to replace) |
| `db-users.py` | after an import: creates the `trinity`, `signup` and `backup` users |
| `build.ps1` | configures (first time or `-Reconfigure`) and builds with Visual Studio 2022 |
| `deploy.ps1` | after every build: copies the build and runtime DLLs into `server\` |
| `configure.ps1` | after every deploy or config change: writes `server\*.conf` |
| `map-data.ps1` | once per client-data change: maps, vmaps, mmaps, Cameras, dbc |
| `run-server.ps1 -Name worldserver` | runs a server in its own console, restarting it after a crash or `.server restart` |
| `bootstrap/install-patches.py` | installs the patch zips from `web\downloads\patches` into a client |
