# DOA2 Ultimate Native Port

The product is a native Windows, macOS and Linux host shell for Dead or Alive
2 Ultimate on Xbox. The first scope is the offline DOA2 portion of Ultimate
Disc 2. DOA1
Ultimate, online services and dashboard/update executables are separate work.

The host shell contains game-independent code and public-safe tooling. The
game import transforms a user-owned disc into a private payload. Executables,
generated game logic, assets, saves and local evidence stay under `private/`.

The whole-program recomp is a temporary scaffold. The final goal is readable,
hand-written game-owned logic with native kernel, input, audio and graphics
services. Replace statically linked libraries at their API boundary.

Feature complete means every offline mode, media path and save flow on a
verified disc revision works. Synthetic model tests establish only their
contracts. They do not establish game compatibility or functional parity.
