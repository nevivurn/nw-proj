{ stdenv, libnsl, libxcrypt }:

stdenv.mkDerivation (finalAttrs: {
  name = "nw-proj3";
  src = ./.;

  enableParallelBuilding = true;

  buildInputs = [ libnsl libxcrypt ];

  installPhase = ''
    runHook preInstall
    install -Dm755 -t $out/bin client server
    runHook postInstall
  '';
})
