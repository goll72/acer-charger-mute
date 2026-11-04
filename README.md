acer-charger-mute
=================

A Linux kernel module that suppresses the beep sound Acer laptops play
when a charger is plugged in or unplugged, by temporarily muting the
speaker amplifier.

It intercepts ACPI EC query `0x38`, mutes the Conexant speaker amplifier for a
short time, then evaluates the original firmware `_Q38` AML method so its side
effects are preserved.

> [!NOTE]
>
> This module was written for the Acer Aspire Go 15 (AG15-71P). If your model
> differs, you will need to tweak the `ACER_*` constants at the top of
> `acer_charger_mute.c` (codec vendor ID, speaker NID, EC query number).
>
> The default mute duration and delay values have been tested to work in the
> aforementioned hardware, while being as small as posible. On other models,
> you might have to tweak the values of the `mute_ms` and `mute_delay_ms`
> module parameters to ensure the beep is suppressed.

## NixOS

Add the flake as an input and import its module; it adds an overlay exposing
`pkgs.acer-charger-mute`, the kernel module built against your running kernel
(`boot.kernelPackages`):

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    acer-charger-mute.url = "github:goll72/acer-charger-mute";
  };

  outputs = inputs@{ nixpkgs, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      modules = [
        inputs.acer-charger-mute.nixosModules.default
        ({ pkgs, ... }: {
          boot.extraModulePackages = [ pkgs.acer-charger-mute ];
          boot.kernelModules = [ "acer_charger_mute" ];

          # Optional hardware-specific override:
          boot.extraModprobeConfig = "options acer_charger_mute mute_ms=800";
        })
      ];
    };
  };
}
```

## Other distros (DKMS)

On other distros, DKMS can be used:

```sh
sudo dkms add .
sudo dkms build acer-charger-mute/1.0
sudo dkms install acer-charger-mute/1.0

sudo modprobe acer_charger_mute
```

To uninstall:

```sh
sudo dkms remove acer-charger-mute/1.0 --all
```

## AI Usage Disclaimer

Most kernel module and packaging infrastructure code and documentation
in this repository is AI-generated, but it has been reviewed, refactored
and tested by me.
