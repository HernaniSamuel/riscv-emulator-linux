$archive = 'linux-6.1.14-rv32nommu-cnl-1.zip';

if( !(Test-Path -Path Image) )
{
	Invoke-WebRequest -Uri https://github.com/cnlohr/mini-rv32ima-images/raw/master/images/$archive -UseBasicParsing -OutFile $archive;
	Expand-Archive linux-6.1.14-rv32nommu-cnl-1.zip -DestinationPath . -ErrorAction SilentlyContinue;
}

$compiler = "gcc";

if( !(Get-Command $compiler -ErrorAction SilentlyContinue) )
{
	Write-Host 'No GCC. Please install it via winget: winget install GnuWin32.Make or via MSYS2: https://www.msys2.org';
	exit 1;
}

# Single-file build: mini-rv32ima.c now contains everything (no separate .h needed)
& $compiler mini-rv32ima.c -o mini-rv32ima.exe

if( $? )
{
	.\mini-rv32ima.exe -f Image
}