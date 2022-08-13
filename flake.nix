{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable-small";
    flake-utils.url = "github:numtide/flake-utils";
  };
  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachSystem [ "x86_64-linux" ]
      (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        with pkgs;{
          devShells.default = mkShell {
            strictDeps = true;
            nativeBuildInputs = [
              yarn
              pkg-config
            ] ++ (with rustPlatform; [
              rust.cargo
              rust.rustc
            ]);
            buildInputs = [
              openssl
              dbus
              glib
              webkitgtk
            ];
            OPENSSL_NO_VENDOR = true;
          };
        });
}
