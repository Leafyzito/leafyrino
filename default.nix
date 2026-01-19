{
  pkgs ? import <nixpkgs> { },
  gitHash ? "",
}:

let
  chatterino2 = pkgs.chatterino2;
in

(chatterino2.buildChatterino {
  enableAvifSupport = true;
}).overrideAttrs
  (
    finalAttrs: prev: {
      pname = "leafyrino";
      version = "unstable";

      buildInputs = prev.buildInputs ++ [
        pkgs.hunspell
      ];

      src = pkgs.nix-gitignore.gitignoreSourcePure [
        "*build-*/"
        "result"
        "*-source/"
        ".git"
      ] ./.;

      patches = [
        ./.patches/nix-cmake-date.patch
      ];

      preConfigure = ''
        export GIT_HASH="${builtins.substring 0 9 gitHash}"
        export GEN_DATE=$(date -I)
        echo GIT_HASH=$GIT_HASH
        echo GEN_DATE=$GEN_DATE
      '';

      cmakeFlags = prev.cmakeFlags ++ [
        (pkgs.lib.cmakeBool "CHATTERINO_SPELLCHECK" true)
      ];

      postInstall = ''
        echo "nightly" > $out/bin/modes
      '';
    }
  )
