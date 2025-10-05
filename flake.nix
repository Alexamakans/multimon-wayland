{
  description = "Viture AR desktop (Wayland DMA-BUF, EGLImage fast path)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };

        pname = "viture_ar_desktop_wayland_dmabuf";
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          inherit pname;
          version = "git";
          src = ./.;

          # Let Nix do the heavy lifting in fixupPhase (patch RPATHs automatically)
          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
            wayland-scanner
            autoPatchelfHook # ← auto-patch $out/bin/* and $out/lib/* using buildInputs
          ];

          # All runtime libs go here so fixupPhase can discover them and add to RUNPATH.
          buildInputs = with pkgs; [
            # Wayland + EGL + GL stack
            glfw-wayland
            wayland
            wayland-protocols # (header-only; still fine in buildInputs)
            libxkbcommon
            libdrm
            libgbm
            libGL
            mesa
            mesa_glu
            egl-wayland

            # Vendor SDK deps seen missing at runtime
            zlib
            libffi
            stdenv.cc.cc.lib # provides libstdc++.so.6 & libgcc_s.so.1

            # for referencing share/wlr-protocols
            wlroots
            wlr-protocols
          ];

          configurePhase = ''
            runHook preConfigure
            cmake -B build -S . \
              -DCMAKE_BUILD_TYPE=RelWithDebInfo \
              -DCMAKE_INSTALL_PREFIX=$out \
              -DWLR_PROTOCOLS_DIR=${pkgs.wlr-protocols}/share/wlr-protocols
            runHook postConfigure
          '';

          buildPhase = ''
            runHook preBuild
            cmake --build build -j$NIX_BUILD_CORES
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            install -Dm755 build/${pname} $out/bin/${pname}

            # If the repo ships the vendor SDK .so, install it so it's in our closure.
            if [ -f libs/libviture_one_sdk.so ]; then
              mkdir -p $out/lib
              install -Dm755 libs/libviture_one_sdk.so $out/lib/libviture_one_sdk.so
            fi
            runHook postInstall
          '';

          # Manual bit: ensure the main binary's RUNPATH includes $out/lib so it can find the SDK.
          # Let autoPatchelfHook handle the rest of the dependency RPATHs from buildInputs.
          postFixup = ''
            if [ -x "$out/bin/${pname}" ]; then
              ${pkgs.patchelf}/bin/patchelf --add-rpath "$out/lib" "$out/bin/${pname}" || true
            fi
          '';

          meta = {
            description =
              "Wayland DMA-BUF capture → EGLImage → OpenGL texture (zero copy)";
            mainProgram = pname;
            platforms = pkgs.lib.platforms.linux;
          };
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/${pname}";
        };

        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            # toolchain
            cmake
            pkg-config
            gcc
            gdb

            # GL/Wayland deps for building & debugging
            glfw-wayland
            wayland
            wayland-protocols
            wayland-scanner
            libxkbcommon
            libdrm
            libgbm
            libGL
            mesa
            mesa_glu
            egl-wayland
            libffi
            zlib

            # diagnostics
            mesa-demos
            vulkan-tools
            lddtree
          ];

          shellHook = ''
            echo "Dev shell ready. Build with:  cmake -B build -S . && cmake --build build -j"
            echo "Run with:           ./build/${pname}"
          '';
        };
      });
}
