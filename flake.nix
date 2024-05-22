{
  outputs = { self, flake-utils, nixpkgs }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = nixpkgs.legacyPackages.${system};
      in {
        packages.default = pkgs.callPackage ./. { };
        devShells.default = pkgs.callPackage ./shell.nix {
          nw-proj = self.packages.${system}.default;
        };
      });
}
