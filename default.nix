# Plain (non-flake) Nix build for ipatool-cpp-sms
{ pkgs ? import <nixpkgs> {} }:

pkgs.stdenv.mkDerivation {
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

  doCheck = false;

  meta = with pkgs.lib; {
    description = "Download App Store IPA packages (SMS 2FA support)";
    license = with licenses; [ asl20 mit ];
    platforms = platforms.unix;
    mainProgram = "ipatool";
  };
}
