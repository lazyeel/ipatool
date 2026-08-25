{
  description = "ipatool-cpp with SMS 2FA — download App Store IPAs without trusted Apple devices";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    let
      # Overlay: makes `ipatool-cpp-sms` (and its alias `ipatool`) available
      # in any nixpkgs consumer: nixos-config, home-manager, nix-shell -p.
      overlay = final: prev: {
        ipatool-cpp-sms = final.callPackage ./default.nix {
          pkgs = final;
        };
      };
    in
    {
      overlays.default = overlay;
      nixosModules.default = { pkgs, lib, ... }: {
        imports = [ ./nixos-module.nix ];
        programs.ipatool-cpp-sms.package = pkgs.callPackage ./default.nix {};
        nixpkgs.overlays = [ overlay ];
      };
    } // flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "ipatool-cpp-sms";
          version = "2.3.2-sms.1";

          src = ./.;

          nativeBuildInputs = with pkgs; [ cmake gnumake pkg-config ];
          buildInputs = with pkgs; [
            openssl
            curl
            zlib
            minizip
            nlohmann_json
          ];

          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

          # nlohmann_json in buildInputs provides the CMake CONFIG package, so
          # find_package(nlohmann_json CONFIG QUIET) succeeds and the fallback
          # file(DOWNLOAD) never fires. minizip resolves through pkg-config
          # (nixpkgs' legacy minizip installs minizip.pc). Dynamic linking is
          # intentional: the Nix store already gives hermetic closure, and
          # STATIC_BUILD pulls in distro-specific static-libcrypto quirks.
          doCheck = false;

          meta = with pkgs.lib; {
            description = "Download App Store IPA packages (SMS 2FA support, no Apple device needed)";
            homepage = "https://github.com/lazyeel/ipatool";
            license = with licenses; [ asl20 mit ];
            platforms = platforms.unix;
            mainProgram = "ipatool";
          };
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/ipatool";
        };
      });
}
