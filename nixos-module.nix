# Copyright 2026 lazyeel (https://github.com/lazyeel)
# SPDX-License-Identifier: Apache-2.0

# NixOS integration: overlay + home-manager / NixOS module examples.
#
# 1. Overlay (makes `ipatool` available to all nixpkgs consumers):
#      nixpkgs.overlays = [ (final: prev: {
#        ipatool-cpp-sms = final.callPackage ./default.nix {};
#      }) ];
#
# 2. NixOS module: declare it once and `ipatool` lands on the system PATH:
#      imports = [ ./nixos-module.nix ];
#      programs.ipatool-cpp-sms.enable = true;
#
# 3. home-manager:
#      home.packages = [ inputs.ipatool-sms.packages.${pkgs.system}.default ];
#
# The account file lives in ~/.ipatool/ and is machine-bound; no service or
# state directory is needed, so the module only manages the package on PATH.
{ config, lib, pkgs, ... }:

with lib;

let
  cfg = config.programs.ipatool-cpp-sms;
in
{
  options.programs.ipatool-cpp-sms = {
    enable = mkEnableOption "ipatool-cpp-sms (App Store IPA downloader with SMS 2FA)";

    package = mkOption {
      type = types.package;
      default = pkgs.callPackage ./default.nix {};
      defaultText = literalExpression "pkgs.callPackage ./default.nix {}";
      description = "The ipatool-cpp-sms package to install.";
    };
  };

  config = mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];
  };
}
