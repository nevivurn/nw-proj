{ stdenv, libnsl }:

stdenv.mkDerivation (finalAttrs: {
  name = "nw-proj4";
  src = ./.;
  buildInputs = [ libnsl ];

  installPhase = ''
    runHook preInstall
    install -Dm755 sr $out/bin/sr
    runHook postInstall
  '';
})
