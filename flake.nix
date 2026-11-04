{
  description = "Acer charger plug/unplug beep suppression kernel module";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      lib = nixpkgs.lib;

      # nixpkgs 26.11+ requires a .ko.cer next to every external module.
      # Older nixpkgs has no kmodigest, so signing is skipped there.
      signModules =
        buildPackages:
        lib.optionalString (buildPackages ? kmodigest) ''
          find $out/lib/modules -name '*.ko' -exec ${lib.getExe buildPackages.kmodigest} {} \;
        '';

      module =
        {
          buildPackages,
          kernel,
        }:
        kernel.stdenv.mkDerivation {
          pname = "acer-charger-mute";
          version = kernel.version;

          src = self;

          nativeBuildInputs = kernel.moduleBuildDependencies;
          hardeningDisable = [
            "pic"
            "format"
          ];

          # XXX: There might be a better way to do this
          buildPhase = ''
            make \
              KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
              KCFLAGS="-Wno-unused-command-line-argument" \
              ${lib.escapeShellArgs (kernel.extraMakeFlags or [ ])}
          '';

          installPhase = ''
            install -D acer_charger_mute.ko \
              $out/lib/modules/${kernel.modDirVersion}/updates/acer_charger_mute.ko
          '';

          postFixup = signModules buildPackages;

          meta = {
            description = "Acer charger plug/unplug beep suppression kernel module";
            license = lib.licenses.gpl2Only;
            platforms = [ "x86_64-linux" ];
          };
        };
    in
    {
      nixosModules.default =
        { config, ... }:
        {
          nixpkgs.overlays = [
            (final: prev: {
              acer-charger-mute = config.boot.kernelPackages.callPackage module { };
            })
          ];
        };
    };
}
