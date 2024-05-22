{ stdenv, libnsl, libxcrypt }:

stdenv.mkDerivation (finalAttrs: {
  name = "nw-proj3";
  src = ./.;

  enableParallelBuilding = true;

  installPhase = ''
    runHook preInstall
    install -Dm755 -t $out/bin client server
    runHook postInstall
  '';
})
