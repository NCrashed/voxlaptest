{
  description = "voxlap fork — portable build (Stage 4.7+)";

  inputs.nixpkgs.url = "flake:nixpkgs";

  outputs = { self, nixpkgs }:
    let
      forAllSystems = f:
        nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ]
          (system: f { pkgs = import nixpkgs { inherit system; }; });
    in {
      devShells = forAllSystems ({ pkgs }: {
        default = pkgs.mkShell {
          packages = with pkgs; [ cmake ninja gcc clang ];
        };
      });

      packages = forAllSystems ({ pkgs }: {
        default = pkgs.stdenv.mkDerivation {
          pname = "voxlap";
          version = "0.1.0";
          src = ./.;
          nativeBuildInputs = with pkgs; [ cmake ninja ];
          # Engine-only build: skip the oracle target so we don't need
          # to ship test fixtures into the Nix store. The library +
          # oracle binary still build during `nix develop` workflows.
          installPhase = ''
            mkdir -p $out/lib $out/bin
            cp bin/libvoxlap.so $out/lib/ 2>/dev/null || true
            cp bin/oracle $out/bin/ 2>/dev/null || true
          '';
        };
      });
    };
}
