// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2020-2022 Mark E Sowden <hogsy@oldtimes-software.com>

#include "renderer.h"
#include "shader_manager.h"

chr::renderer::IShaderProgram *chr::renderer::IShaderManager::FetchProgram( const std::string &name )
{
	auto i = programs.find( name );
	if ( i == programs.end() )
	{
		Com_Printf( "Failed to find program, \"%s\"!\n", name.c_str() );
		return nullptr;
	}

	return i->second;
}
