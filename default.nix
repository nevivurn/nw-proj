{
  stdenv,
  libnsl,
  pandoc,
  texliveSmall,
}:

stdenv.mkDerivation (finalAttrs: {
  name = "nw-proj4";
  src = ./.;

  nativeBuildInputs = [
    pandoc
    texliveSmall
  ];
  buildInputs = [ libnsl ];

  installPhase = ''
    runHook preInstall
    install -Dm755 sr $out/bin/sr
    runHook postInstall
  '';
})
