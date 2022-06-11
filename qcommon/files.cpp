// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright © 1997-2001 Id Software, Inc.
// Copyright © 2020-2022 Mark E Sowden <hogsy@oldtimes-software.com>

#include <iostream>
#include <fstream>

#include "qcommon.h"

#include <sys/stat.h>
#include <miniz/miniz.h>

/*
=============================================================================

QUAKE FILESYSTEM

=============================================================================
*/

static bool FS_DecompressFile( const uint8_t *srcBuffer, size_t srcLength, uint8_t *dstBuffer, size_t *dstLength, size_t expectedLength );

/*
=============================================================================
Anachronox Data Packages
=============================================================================
*/

static constexpr unsigned int ADAT_MAGIC = GENERATE_MAGICID( 'A', 'D', 'A', 'T' );
static constexpr unsigned int ADAT_VERSION = 9;

struct Package
{
	struct Index
	{
		char     name[ 128 ];      /* the name of the file, excludes 'model/' etc. */
		uint32_t offset;           /* offset into the dat that the file resides */
		uint32_t length;           /* decompressed length of the file */
		uint32_t compressedLength; /* length of the file in the dat */
		uint32_t u0;
	};

	std::string          mappedDir; /* e.g., 'models' */
	std::string          path;
	std::vector< Index > indices; /* index data */

	/**
 	 * Search for the given file within a package and return it's index.
 	 */
	const Index *GetFileIndex( const char *fileName ) const
	{
		for ( const auto &indice : indices )
		{
			const Index *index = &indice;
			if ( Q_strncasecmp( index->name, fileName, sizeof( index->name ) ) != 0 )
				continue;

			return index;
		}

		return nullptr;
	}

	/**
 	 * Load a file from the given package, and decompress the data.
 	 */
	uint8_t *LoadFile( const char *fileName, uint32_t *fileLength ) const
	{
		// first, ensure that it's actually in the package file table
		const Index *fileIndex = GetFileIndex( fileName );
		if ( fileIndex == nullptr )
			return nullptr;

		// now proceed to load that file

		std::ifstream file;
		file.open( path, std::ios::binary );
		if ( !file.is_open() )
		{
			Com_Printf( "WARNING: Failed to open package \"%s\"!\n", path.c_str() );
			return nullptr;
		}

		file.seekg( fileIndex->offset );

		// now load the compressed block in
		std::vector< uint8_t > src( fileIndex->compressedLength );
		file.read( ( char * ) src.data(), fileIndex->compressedLength );

		// decompress it
		auto   dst = new uint8_t[ fileIndex->length ];
		size_t dstLength = fileIndex->length;
		bool   status = FS_DecompressFile( src.data(), fileIndex->compressedLength, dst, &dstLength, fileIndex->length );

		file.close();

		if ( status )
		{
			*fileLength = dstLength;
			return dst;
		}

		delete[] dst;

		return nullptr;
	}
};

/*
=============================================================================
=============================================================================
*/

/**
 * Convert back-slashes to forward slashes, to play nice with our packages.
 */
void FS_CanonicalisePath( char *path )
{
	while ( *path != '\0' )
	{
		if ( *path == '\\' )
			*path = '/';

		path++;
	}
}

/**
 * Decompress the given file and carry out validation.
 */
static bool FS_DecompressFile( const uint8_t *srcBuffer, size_t srcLength, uint8_t *dstBuffer, size_t *dstLength, size_t expectedLength )
{
	int returnCode = mz_uncompress( dstBuffer, ( mz_ulong * ) dstLength, srcBuffer, ( mz_ulong ) srcLength );
	if ( returnCode != MZ_OK )
	{
		Com_Printf( "Failed to decompress data, return code \"%d\"!\n", returnCode );
		return false;
	}

	if ( *dstLength != expectedLength )
	{
		Com_Printf( "Unexpected size following decompression, %d vs %d!\n", *dstLength, expectedLength );
		return false;
	}

	return true;
}

static bool FS_MountPackage( FILE *filePtr, const char *identity, Package *out )
{
	if ( identity == nullptr || identity[ 0 ] == '\0' )
	{
		Com_Printf( "WARNING: Invalid package identity!\n" );
		return false;
	}

	struct PackageHeader
	{
		uint32_t magic;     /* ADAT */
		uint32_t tocOffset; /* table of contents */
		uint32_t tocLength; /* table of contents length */
		uint32_t version;   /* always appears to be 9 */
	} header{};

	// read in the header
	fread( &header, sizeof( PackageHeader ), 1, filePtr );

	// and now ensure it's as desired!
	header.magic = LittleLong( ( int ) header.magic );
	if ( header.magic != ADAT_MAGIC )
	{
		Com_Printf( "WARNING: Invalid magic, expected \"ADAT\"!\n" );
		return false;
	}

	header.version = LittleLong( ( int ) header.version );
	if ( header.version != 9 )
	{
		Com_Printf( "WARNING: Unexpected package version, \"%d\" (expected \"9\")!\n", header.version );
		return false;
	}

	unsigned int numFiles = header.tocLength / sizeof( Package::Index );
	if ( numFiles == 0 )
	{
		Com_Printf( "WARNING: Empty package!\n" );
		return false;
	}

	auto package = new Package();
	out->mappedDir = identity;

	// seek to the table of contents
	fseek( filePtr, ( long ) header.tocOffset, SEEK_SET );

	// and now read in the table of contents
	out->indices.resize( numFiles );
	if ( fread( out->indices.data(), sizeof( Package::Index ), numFiles, filePtr ) != numFiles )
	{
		Com_Printf( "WARNING: Failed to read entire table of contents!\n" );
		return false;
	}

	// flip back slash to forward
	for ( auto &indice : out->indices )
		FS_CanonicalisePath( indice.name );

	return true;
}

//
// in memory
//

static char    fs_gamedir[ MAX_OSPATH ];
static cvar_t *fs_basedir;
static cvar_t *fs_cddir;

cvar_t *fs_gamedirvar;

struct searchpath_t
{
	char                             filename[ MAX_OSPATH ]{ '\0' };
	std::map< std::string, Package > packDirectories;
	struct searchpath_t             *next{ nullptr };
};

static searchpath_t *fs_searchpaths;
static searchpath_t *fs_base_searchpaths;// without gamedirs


/*

All of Quake's data access is through a hierchal file system, but the contents of the file system can be transparently merged from several sources.

The "base directory" is the path to the directory holding the quake.exe and all game directories.  The sys_* files pass this to host_init in quakeparms_t->basedir.  This can be overridden with the "-basedir" command line parm to allow code debugging in a different directory.  The base directory is
only used during filesystem initialization.

The "game directory" is the first tree on the search path and directory that all generated files (savegames, screenshots, demos, config files) will be saved to.  This can be overridden with the "-game" command line parameter.  The game directory can never be changed while quake is executing.  This is a precacution against having a malicious server instruct clients to write files over areas they shouldn't.

*/

/**
 * Stat a file to fetch it's size. Only works for local files.
 */
long FS_GetLocalFileLength( const char *path )
{
	struct stat buf{};
	if ( stat( path, &buf ) != 0 )
		return -1;

	return buf.st_size;
}

/*
============
FS_CreatePath

Creates any directories needed to store the given filename
============
*/
void FS_CreatePath( char *path )
{
	for ( char *ofs = path + 1; *ofs; ofs++ )
	{
		if ( *ofs == '/' )
		{// create the directory
			*ofs = 0;
			Sys_Mkdir( path );
			*ofs = '/';
		}
	}
}

/**
 * Check if the given file exists locally. Only works for local files.
 */
bool FS_LocalFileExists( const char *path )
{
	return ( FS_GetLocalFileLength( path ) != -1 );
}

/*
===========
FS_FOpenFile

Finds the file in the search path.
returns filesize and an open FILE *
Used for streaming data out of either a pak file or
a seperate file.
===========
*/
void *FS_FOpenFile( const char *filename, uint32_t *length )
{
	// search through the path, one element at a time
	for ( searchpath_t *search = fs_searchpaths; search; search = search->next )
	{
		// check a file in the directory tree
		char netpath[ MAX_OSPATH ];
		Com_sprintf( netpath, sizeof( netpath ), "%s/%s", search->filename, filename );

		FS_CanonicalisePath( netpath );

		// first, attempt to open it locally
		uint32_t fileLength = FS_GetLocalFileLength( netpath );
		if ( fileLength >= 0 )
		{
			FILE *filePtr = fopen( netpath, "rb" );
			if ( filePtr != nullptr )
			{
				/* allocate a buffer and read the whole thing into memory */
				void *buffer = Z_Malloc( fileLength );
				fread( buffer, sizeof( uint8_t ), fileLength, filePtr );

				fclose( filePtr );
				*length = fileLength;

				return buffer;
			}
		}

		// otherwise, load it from one of the anox packages

		/* Anachronox does some horrible path munging in order to determine which package
		 * it should be loading from, so we need to grab the first dir here to do the same. */
		char rootFolder[ 32 ];
		memset( rootFolder, 0, sizeof( rootFolder ) );
		const char *p = filename;
		for ( unsigned int i = 0; i < sizeof( rootFolder ) - 4; ++i )
		{
			if ( *p == '\0' || *p == '/' )
				break;

			rootFolder[ i ] = *p++;
		}
		p++;

		// see if we have a match!
		for ( const auto& i : search->packDirectories )
		{
			if ( i.second.mappedDir != rootFolder )
				continue;

			uint8_t *buffer = i.second.LoadFile( p, length );
			if ( buffer == nullptr )
				break;

			return buffer;
		}
	}

	Com_DPrintf( "FindFile: can't find %s\n", filename );

	return nullptr;
}

/*
=================
FS_ReadFile

Properly handles partial reads
=================
*/
void CDAudio_Stop();
#define MAX_READ 0x10000// read in blocks of 64k
void FS_Read( void *buffer, int len, FILE *f )
{
	int   block, remaining;
	int   read;
	byte *buf;
	int   tries;

	buf = ( byte * ) buffer;

	// read in chunks for progress bar
	remaining = len;
	tries = 0;
	while ( remaining )
	{
		block = remaining;
		if ( block > MAX_READ )
			block = MAX_READ;
		read = fread( buf, 1, block, f );
		if ( read == 0 )
		{
			// we might have been trying to read from a CD
			if ( !tries )
			{
				tries = 1;
				CDAudio_Stop();
			}
			else
				Com_Error( ERR_FATAL, "FS_Read: 0 bytes read" );
		}

		if ( read == -1 )
			Com_Error( ERR_FATAL, "FS_Read: -1 bytes read" );

		// do some progress bar thing here...

		remaining -= read;
		buf += read;
	}
}

/*
============
FS_LoadFile

Filename are reletive to the quake search path
a null buffer will just return the file length without loading
============
*/
int FS_LoadFile( const char *path, void **buffer )
{
	char upath[ MAX_QPATH ];
	snprintf( upath, sizeof( upath ), "%s", path );

	FS_CanonicalisePath( upath );

	// look for it in the filesystem or pack files
	uint32_t length;
	void *buf = FS_FOpenFile( upath, &length );
	if ( buf == nullptr )
	{
		if ( buffer != nullptr )
			*buffer = nullptr;

		return -1;
	}

	/* retain compat for fetching the length, for now */
	if ( buffer == nullptr )
	{
		Z_Free( buf );
		return length;
	}

	*buffer = buf;

	return length;
}

void FS_FreeFile( void *buffer )
{
	Z_Free( buffer );
}

/*
================
FS_AddGameDirectory

Sets fs_gamedir, adds the directory to the head of the path,
then loads and adds pak1.pak pak2.pak ...
================
*/
static void FS_AddGameDirectory( const char *dir )
{
	strcpy( fs_gamedir, dir );

	//
	// add the directory to the search path
	//
	auto search = new searchpath_t;
	strcpy( search->filename, dir );
	search->next = fs_searchpaths;
	fs_searchpaths = search;

	/* now go ahead and mount all the default packages under that dir */

	static const char *defaultPacks[] = {
			"battle",
			"gameflow",
			"graphics",
			"maps",
			"models",
			"objects",
			"particles",
			"scripts",
			"sound",
			"sprites",
			"textures",
	};

	for ( auto &defaultPack : defaultPacks )
	{
		/* check a file in the directory tree, e.g. 'anoxdata/battle.dat' */
		std::string packPath = search->filename;
		packPath += "/" + std::string( defaultPack ) + ".dat";

		FILE *filePtr = fopen( packPath.c_str(), "rb" );
		if ( filePtr == nullptr )
			continue;

		Package package;
		if ( FS_MountPackage( filePtr, defaultPack, &package ) )
		{
			/* if it loaded successfully, add it onto the list */
			package.path = packPath;
			search->packDirectories.emplace( defaultPack, package );
		}

		fclose( filePtr );
	}
}

/*
============
FS_Gamedir

Called to find where to write a file (demos, savegames, etc)
============
*/
const char *FS_Gamedir()
{
	if ( *fs_gamedir )
		return fs_gamedir;
	else
		return BASEDIRNAME;
}

/*
=============
FS_ExecAutoexec
=============
*/
void FS_ExecAutoexec()
{
	char *dir;
	char  name[ MAX_QPATH ];

	dir = Cvar_VariableString( "gamedir" );
	if ( *dir )
		Com_sprintf( name, sizeof( name ), "%s/%s/autoexec.cfg", fs_basedir->string, dir );
	else
		Com_sprintf( name, sizeof( name ), "%s/%s/autoexec.cfg", fs_basedir->string, BASEDIRNAME );
	if ( Sys_FindFirst( name, 0, SFF_SUBDIR | SFF_HIDDEN | SFF_SYSTEM ) )
		Cbuf_AddText( "exec autoexec.cfg\n" );
	Sys_FindClose();
}

/*
================
FS_SetGamedir

Sets the gamedir and path to a different directory.
================
*/
void FS_SetGamedir( const char *dir )
{
	searchpath_t *next;

	if ( strstr( dir, ".." ) || strstr( dir, "/" ) || strstr( dir, "\\" ) || strstr( dir, ":" ) )
	{
		Com_Printf( "Gamedir should be a single filename, not a path\n" );
		return;
	}

	//
	// free up any current game dir info
	//
	while ( fs_searchpaths != fs_base_searchpaths )
	{
		next = fs_searchpaths->next;
		Z_Free( fs_searchpaths );
		fs_searchpaths = next;
	}

	//
	// flush all data, so it will be forced to reload
	//
	if ( dedicated && !dedicated->value )
		Cbuf_AddText( "vid_restart\nsnd_restart\n" );

	Com_sprintf( fs_gamedir, sizeof( fs_gamedir ), "%s/%s", fs_basedir->string, dir );

	if ( !strcmp( dir, BASEDIRNAME ) || ( *dir == 0 ) )
	{
		Cvar_FullSet( "gamedir", "", CVAR_SERVERINFO | CVAR_NOSET );
		Cvar_FullSet( "game", "", CVAR_LATCH | CVAR_SERVERINFO );
	}
	else
	{
		Cvar_FullSet( "gamedir", dir, CVAR_SERVERINFO | CVAR_NOSET );
		if ( fs_cddir->string[ 0 ] )
			FS_AddGameDirectory( va( "%s/%s", fs_cddir->string, dir ) );

		FS_AddGameDirectory( va( "%s/%s", fs_basedir->string, dir ) );
	}
}

/*
** FS_ListFiles
*/
char **FS_ListFiles( char *findname, int *numfiles, unsigned musthave, unsigned canthave )
{
	char  *s;
	int    nfiles = 0;
	char **list = nullptr;

	s = Sys_FindFirst( findname, musthave, canthave );
	while ( s )
	{
		if ( s[ strlen( s ) - 1 ] != '.' )
			nfiles++;
		s = Sys_FindNext( musthave, canthave );
	}
	Sys_FindClose();

	if ( !nfiles )
		return nullptr;

	nfiles++;// add space for a guard
	*numfiles = nfiles;

	list = ( char ** ) malloc( sizeof( char * ) * nfiles );
	memset( list, 0, sizeof( char * ) * nfiles );

	s = Sys_FindFirst( findname, musthave, canthave );
	nfiles = 0;
	while ( s )
	{
		if ( s[ strlen( s ) - 1 ] != '.' )
		{
			list[ nfiles ] = Q_strdup( s );
			Q_strtolower( list[ nfiles ] );
			nfiles++;
		}
		s = Sys_FindNext( musthave, canthave );
	}
	Sys_FindClose();

	return list;
}

/*
** FS_Dir_f
*/
void FS_Dir_f()
{
	char  *path = nullptr;
	char   findname[ 1024 ];
	char   wildcard[ 1024 ] = "*.*";
	char **dirnames;
	int    ndirs;

	if ( Cmd_Argc() != 1 )
		strcpy( wildcard, Cmd_Argv( 1 ) );

	while ( ( path = FS_NextPath( path ) ) != nullptr )
	{
		char *tmp = findname;

		Com_sprintf( findname, sizeof( findname ), "%s/%s", path, wildcard );

		while ( *tmp != 0 )
		{
			if ( *tmp == '\\' )
				*tmp = '/';
			tmp++;
		}
		Com_Printf( "Directory of %s\n", findname );
		Com_Printf( "----\n" );

		if ( ( dirnames = FS_ListFiles( findname, &ndirs, 0, 0 ) ) != 0 )
		{
			int i;

			for ( i = 0; i < ndirs - 1; i++ )
			{
				if ( strrchr( dirnames[ i ], '/' ) )
					Com_Printf( "%s\n", strrchr( dirnames[ i ], '/' ) + 1 );
				else
					Com_Printf( "%s\n", dirnames[ i ] );

				free( dirnames[ i ] );
			}
			free( dirnames );
		}
		Com_Printf( "\n" );
	};
}

/*
============
FS_Path_f

============
*/
void FS_Path_f()
{
	Com_Printf( "Current search path:\n" );
	for ( searchpath_t *s = fs_searchpaths; s; s = s->next )
	{
		if ( s == fs_base_searchpaths )
			Com_Printf( "----------\n" );

		Com_Printf( "%s\n", s->filename );

		/* list out all the mounted packages */

		Com_Printf( "Packages:\n" );
		for ( const auto &i : s->packDirectories )
			Com_Printf( " %s\n", i.second.mappedDir.c_str() );
	}
}

/*
================
FS_NextPath

Allows enumerating all of the directories in the search path
================
*/
char *FS_NextPath( char *prevpath )
{
	if ( !prevpath )
		return fs_gamedir;

	char *prev = fs_gamedir;
	for ( searchpath_t *s = fs_searchpaths; s; s = s->next )
	{
		if ( prevpath == prev )
			return s->filename;

		prev = s->filename;
	}

	return nullptr;
}

/*
================
FS_InitFilesystem
================
*/
void FS_InitFilesystem( void )
{
	Cmd_AddCommand( "path", FS_Path_f );
	Cmd_AddCommand( "dir", FS_Dir_f );

	//
	// basedir <path>
	// allows the game to run from outside the data tree
	//
	fs_basedir = Cvar_Get( "basedir", ".", CVAR_NOSET );

	//
	// cddir <path>
	// Logically concatenates the cddir after the basedir for
	// allows the game to run from outside the data tree
	//
	fs_cddir = Cvar_Get( "cddir", "", CVAR_NOSET );
	if ( fs_cddir->string[ 0 ] )
		FS_AddGameDirectory( va( "%s/" BASEDIRNAME, fs_cddir->string ) );

	//
	// start up with baseq2 by default
	//
	FS_AddGameDirectory( va( "%s/" BASEDIRNAME, fs_basedir->string ) );

	// any set gamedirs will be freed up to here
	fs_base_searchpaths = fs_searchpaths;

	// check for game override
	fs_gamedirvar = Cvar_Get( "game", "", CVAR_LATCH | CVAR_SERVERINFO );
	if ( fs_gamedirvar->string[ 0 ] )
		FS_SetGamedir( fs_gamedirvar->string );
}
