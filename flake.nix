{
  description = "Viture AR desktop (Wayland DMA-BUF, EGLImage fast path)";

  inputs = {
    # You can switch this to follow your own nixpkgs fork/branch if you want.
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

          nativeBuildInputs =
            [ pkgs.cmake pkgs.pkg-config pkgs.wayland-scanner pkgs.patchelf ];

          buildInputs = with pkgs; [
            # Wayland + EGL + GL stack
            glfw-wayland
            wayland
            wayland-protocols
            libdrm
            libgbm
            mesa
            libGL
            mesa_glu
            libxkbcommon
            egl-wayland
            libffi
            zlib # viture sdk .so file depends on this
          ];

          # CMakeLists.txt already generates protocol headers and builds in ./build
          configurePhase = ''
            runHook preConfigure
            cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$out
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

            # Match the target name from CMakeLists.txt
            install -Dm755 build/${pname} $out/bin/${pname}

            # If your repo ships the VITURE SDK .so, install it and set RPATH
            if [ -f libs/libviture_one_sdk.so ]; then
              mkdir -p $out/lib
              install -Dm755 libs/libviture_one_sdk.so $out/lib/libviture_one_sdk.so
              ${pkgs.patchelf}/bin/patchelf \
                --set-rpath "$out/lib:${
                  pkgs.lib.makeLibraryPath [
                    pkgs.glfw-wayland
                    pkgs.libGL
                    pkgs.mesa
                    pkgs.mesa_glu
                    pkgs.egl-wayland
                    pkgs.libgbm
                    pkgs.libdrm
                    pkgs.zlib
                  ]
                }" \
                $out/bin/${pname}
            fi

            runHook postInstall
          '';

          meta = {
            description =
              "Wayland DMA-BUF capture → EGLImage → OpenGL texture (zero copy)";
            mainProgram = pname; # enables `nix run` without specifying program
            platforms = pkgs.lib.platforms.linux;
          };
        };

        # `nix run .` will just start the app
        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/${pname}";
        };

        # Handy dev shell: headers, tools, and GPU/Wayland stack available
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
            libdrm
            libgbm
            mesa
            libGL
            mesa_glu
            libxkbcommon
            egl-wayland
            libffi

            # useful diagnostics
            mesa-demos
            vulkan-tools
          ];

          # Helpful when using render nodes; add yourself to `video` group system-wide if needed
          shellHook = ''
            echo "Dev shell ready. Build with:  cmake -B build -S . && cmake --build build -j"
            echo "Run with:           ./build/${pname}"
          '';
        };
      });
}
