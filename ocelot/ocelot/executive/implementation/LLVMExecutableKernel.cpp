/*!
	\file LLVMExecutableKernel.cpp
	\date Friday September 4, 2009
	\author Gregory Diamos <gregory.diamos@gatech.edu>
	\brief The source file for the LLVMExecutableKernel class
*/

#ifndef LLVM_EXECUTABLE_KERNEL_CPP_INCLUDED
#define LLVM_EXECUTABLE_KERNEL_CPP_INCLUDED

#include <ocelot/executive/interface/LLVMExecutableKernel.h>
#include <ocelot/executive/interface/TextureOperations.h>
#include <hydrazine/implementation/debug.h>
#include <hydrazine/implementation/Exception.h>
#include <ocelot/translator/interface/PTXToLLVMTranslator.h>
#include <ocelot/ir/interface/Module.h>
#include <ocelot/analysis/interface/RemoveBarrierPass.h>
#include <ocelot/analysis/interface/ConvertPredicationToSelectPass.h>
#include <fstream>

#include <hydrazine/implementation/debug.h>

#ifdef REPORT_BASE
#undef REPORT_BASE
#endif

#define REPORT_BASE 0
#define REPORT_ALL_PTX_SOURCE 0
#define REPORT_ORIGINAL_LLVM_SOURCE 0
#define REPORT_OPTIMIZED_LLVM_SOURCE 0
#define REPORT_INSIDE_TRANSLATED_CODE 0
#define REPORT_ATOMIC_OPERATIONS 0
#define PRINT_OPTIMIZED_CFG 0
#define DEBUG_FIRST_THREAD_ONLY 0
#define DEBUG_PTX_INSTRUCTION_TRACE 0
#define DEBUG_PTX_BASIC_BLOCK_TRACE 1

#include <configure.h>

#ifdef HAVE_LLVM
#include <llvm/Analysis/Verifier.h>
#include <llvm/Assembly/Parser.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/JIT.h> 
#include <llvm/Module.h>
#include <llvm/ModuleProvider.h>
#include <llvm/PassManager.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Target/TargetSelect.h>
#include <llvm/Transforms/Scalar.h>
#endif

#include <unordered_set>

extern "C"
{
	void setRoundingMode( unsigned int mode )
	{
		assertM( mode == 0, "No support for setting exotic rounding modes." );
	}
	
	float __ocelot_ex2( float value )
	{
		return exp( value * 0.693147f );
	}

	float __ocelot_rsqrt( float value )
	{
		return 1.0 / sqrt( value );
	}
	
	double __ocelot_sqrt( double f )
	{
		return sqrt( f );
	}
	
	float __ocelot_sqrtf( float f )
	{
		return sqrt( f );
	}
	
	float __ocelot_log2f( float f )
	{
		return log2( f );
	}

	float __ocelot_sinf( float f )
	{
		return sin( f );
	}

	float __ocelot_cosf( float f )
	{
		return cos( f );
	}

	bool __ocelot_vote( bool a, ir::PTXInstruction::VoteMode mode, bool invert )
	{
		a = invert ? !a : a;
		
		switch( mode )
		{
			case ir::PTXInstruction::All:
			case ir::PTXInstruction::Any:
			{
				return a;
				break;
			}
			case ir::PTXInstruction::Uni:
			default: break;
		}
		return true;
	}

	ir::PTXF32 __ocelot_atom_f32( executive::LLVMContext* context, 
		ir::PTXInstruction::AddressSpace space, 
		ir::PTXInstruction::AtomicOperation op, ir::PTXU64 address, ir::PTXF32 b )
	{
		if( space == ir::PTXInstruction::Shared )
		{
			address += ( ir::PTXU64 ) context->shared;
		}
		
		ir::PTXF32 d = 0;
		
		switch( op )
		{
			case ir::PTXInstruction::AtomicAdd:
			{
				d = *((ir::PTXF32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicAdd: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( d + b ) );
				*((ir::PTXF32*) address) = d + b;
				break;
			}
			case ir::PTXInstruction::AtomicMin:
			{
				d = *((ir::PTXF32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicMin: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << std::min( d, b ) );
				*((ir::PTXF32*) address) = std::min( d, b );
				break;
			}
			case ir::PTXInstruction::AtomicMax:
			{
				d = *((ir::PTXF32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicMin: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << std::max( d, b ) );
				*((ir::PTXF32*) address) = std::max( d, b );
				break;
			}
			default: assertM( false, "Atomic " 
				<< ir::PTXInstruction::toString( op ) 
				<< " not supported for f32." );
		}
		
		return d;
	}

	ir::PTXB32 __ocelot_atom_b32( executive::LLVMContext* context, 
		ir::PTXInstruction::AddressSpace space, 
		ir::PTXInstruction::AtomicOperation op, 
		ir::PTXU64 address, ir::PTXB32 b )
	{
		if( space == ir::PTXInstruction::Shared )
		{
			address += ( ir::PTXU64 ) context->shared;
		}
		
		ir::PTXB32 d = 0;
		
		switch( op )
		{
			case ir::PTXInstruction::AtomicAnd:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicAnd: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( d & b ) );
				*((ir::PTXB32*) address) = d & b;
				break;
			}
			case ir::PTXInstruction::AtomicOr:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicOr: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( d | b ) );
				*((ir::PTXB32*) address) = d | b;
				break;
			}
			case ir::PTXInstruction::AtomicXor:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicXor: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( d ^ b ) );
				*((ir::PTXB32*) address) = d ^ b;
				break;
			}
			case ir::PTXInstruction::AtomicAdd:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicAdd: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( d + b ) );
				*((ir::PTXB32*) address) = d + b;
				break;
			}
			case ir::PTXInstruction::AtomicMin:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicMin: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << std::min( d, b ) );
				*((ir::PTXB32*) address) = std::min( d, b );
				break;
			}
			case ir::PTXInstruction::AtomicMax:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicMax: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << std::max( d, b ) );
				*((ir::PTXB32*) address) = std::max( d, b );
				break;
			}
			case ir::PTXInstruction::AtomicDec:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicDec: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( ((d == 0) || (d > b)) ? b : d - 1 ) );
				*((ir::PTXB32*) address) = ((d == 0) || (d > b)) ? b : d - 1;
				break;
			}
			case ir::PTXInstruction::AtomicInc:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicInc: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( (d >= b) ? 0 : d + 1 ) );
				*((ir::PTXB32*) address) = (d >= b) ? 0 : d + 1;
				break;
			}
			case ir::PTXInstruction::AtomicExch:
			{
				d = *((ir::PTXB32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicExch: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << b );
				*((ir::PTXB32*) address) = b;
				break;
			}
			default: assertM( false, "Atomic " 
				<< ir::PTXInstruction::toString( op ) 
				<< " not supported for b32." );
		}
		
		return d;
	}

	ir::PTXS32 __ocelot_atom_s32( executive::LLVMContext* context, 
		ir::PTXInstruction::AddressSpace space, 
		ir::PTXInstruction::AtomicOperation op, 
		ir::PTXU64 address, ir::PTXS32 b )
	{
		if( space == ir::PTXInstruction::Shared )
		{
			address += ( ir::PTXU64 ) context->shared;
		}
		
		ir::PTXS32 d = 0;
		
		switch( op )
		{
			case ir::PTXInstruction::AtomicAdd:
			{
				d = *((ir::PTXS32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicAdd: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( d + b ) );
				*((ir::PTXS32*) address) = d + b;
				break;
			}
			case ir::PTXInstruction::AtomicMin:
			{
				d = *((ir::PTXS32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicMin: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << std::min( d, b ) );
				*((ir::PTXS32*) address) = std::min( d, b );
				break;
			}
			case ir::PTXInstruction::AtomicMax:
			{
				d = *((ir::PTXS32*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicMax: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << std::max( d, b ) );
				*((ir::PTXS32*) address) = std::max( d, b );
				break;
			}
			default: assertM( false, "Atomic " 
				<< ir::PTXInstruction::toString( op ) 
				<< " not supported for s32." );
		}
		
		return d;
	}

	ir::PTXB64 __ocelot_atom_b64( executive::LLVMContext* context, 
		ir::PTXInstruction::AddressSpace space, 
		ir::PTXInstruction::AtomicOperation op, 
		ir::PTXU64 address, ir::PTXB64 b )
	{
		if( space == ir::PTXInstruction::Shared )
		{
			address += ( ir::PTXU64 ) context->shared;
		}
		
		ir::PTXB64 d = 0;
		
		switch( op )
		{
			case ir::PTXInstruction::AtomicAdd:
			{
				d = *((ir::PTXB64*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicAdd: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << ( d + b ) );
				*((ir::PTXB64*) address) = d + b;
				break;
			}
			case ir::PTXInstruction::AtomicExch:
			{
				d = *((ir::PTXB64*) address);
				reportE( REPORT_ATOMIC_OPERATIONS, "AtomicExch: address " 
					<< (void*) address << " from " << d << " by " << b 
					<< " to " << b );
				*((ir::PTXB64*) address) = b;
				break;
			}
			default: assertM( false, "Atomic " 
				<< ir::PTXInstruction::toString( op ) 
				<< " not supported for b64." );
		}
		
		return d;
	}

	ir::PTXB32 __ocelot_atomcas_b32( executive::LLVMContext* context, 
		ir::PTXInstruction::AddressSpace space, 
		ir::PTXInstruction::AtomicOperation op, ir::PTXU64 address, 
		ir::PTXB32 b, ir::PTXB32 c )
	{
		if( space == ir::PTXInstruction::Shared )
		{
			address += ( ir::PTXU64 ) context->shared;
		}
		
		ir::PTXB32 d = *((ir::PTXB32*) address);
		
		assert( op == ir::PTXInstruction::AtomicCas );

		reportE( REPORT_ATOMIC_OPERATIONS, "AtomicCas: address " 
			<< (void*) address << " from " << d << " by " << b 
			<< " to " << ( ( d == b ) ? c : d ) );

		*((ir::PTXB32*) address) = ( d == b ) ? c : d;

		return d;
	}

	ir::PTXB64 __ocelot_atomcas_b64( executive::LLVMContext* context, 
		ir::PTXInstruction::AddressSpace space, 
		ir::PTXInstruction::AtomicOperation op, ir::PTXU64 address, 
		ir::PTXB64 b, ir::PTXB64 c )
	{
		if( space == ir::PTXInstruction::Shared )
		{
			address += ( ir::PTXU64 ) context->shared;
		}
		
		ir::PTXB64 d = *((ir::PTXB64*) address);
		
		assert( op == ir::PTXInstruction::AtomicCas );

		reportE( REPORT_ATOMIC_OPERATIONS, "AtomicCas: address " 
			<< (void*) address << " from " << d << " by " << b 
			<< " to " << ( ( d == b ) ? c : d ) );

		*((ir::PTXB64*) address) = ( d == b ) ? c : d;

		return d;
	}

	unsigned int __ocelot_clock( executive::LLVMContext* context )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		
		return state->timer.cycles();
	}
	
	void __ocelot_debug_block( executive::LLVMContext* context, 
		ir::BasicBlock::Id id )
	{
		#if(DEBUG_PTX_BASIC_BLOCK_TRACE == 1)
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		
		executive::LLVMExecutableKernel::OpaqueState::BlockIdMap::const_iterator
			block = state->blocks.find( id );
		assert( block != state->blocks.end() );
		
		#if(DEBUG_FIRST_THREAD_ONLY == 1)
		if( context->tid.x == 0 && context->tid.y == 0 && context->tid.z == 0 )
		{
		#endif
		
		std::cout << "Thread (" << context->tid.x << ", " << context->tid.y 
			<< ", " << context->tid.z << ") : Basic Block \"" << std::flush;
		std::cout << block->second->label << "\"\n";

		#if(DEBUG_FIRST_THREAD_ONLY == 1)
		}
		#endif
		#endif
	}

	void __ocelot_debug_instruction( executive::LLVMContext* context, 
		unsigned int instruction )
	{
		#if(DEBUG_PTX_INSTRUCTION_TRACE == 1)		
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;

		#if(DEBUG_FIRST_THREAD_ONLY == 1)
		if( context->tid.x == 0 && context->tid.y == 0 && context->tid.z == 0 )
		{
		#endif
		
		std::cout << "Thread (" << context->tid.x << ", " << context->tid.y 
			<< ", " << context->tid.z << ") :  " << std::flush;
		std::cout << (*state->instructions)[instruction].toString() << "\n";

		#if(DEBUG_FIRST_THREAD_ONLY == 1)
		}
		#endif

		#endif
	}
		
	void __ocelot_tex_3d_fs( float* result, executive::LLVMContext* context, 
		unsigned int index, unsigned int c0, unsigned int c1, unsigned int c2,
		unsigned int c3 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, float >( 
			texture, c0, c1, c2 );
		result[1] = executive::tex::sample< 1, float >( 
			texture, c0, c1, c2 );
		result[2] = executive::tex::sample< 2, float >( 
			texture, c0, c1, c2 );
		result[3] = executive::tex::sample< 3, float >( 
			texture, c0, c1, c2 );			
	}

	void __ocelot_tex_3d_fu( float* result, executive::LLVMContext* context, 
		unsigned int index, unsigned int c0, unsigned int c1, unsigned int c2, 
		unsigned int c3 )
	{
		__ocelot_tex_3d_fs( result, context, index, c0, c1, c2, c3 );
	}

	void __ocelot_tex_3d_ff( float* result, executive::LLVMContext* context, 
		unsigned int index, float c0, float c1, float c2, float c3 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, float >( 
			texture, c0, c1, c2 );
		result[1] = executive::tex::sample< 1, float >( 
			texture, c0, c1, c2 );
		result[2] = executive::tex::sample< 2, float >( 
			texture, c0, c1, c2 );
		result[3] = executive::tex::sample< 3, float >( 
			texture, c0, c1, c2 );	
	}

	void __ocelot_tex_3d_sf( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, float c0, 
		float c1, float c2, float c3 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, int >( 
			texture, c0, c1, c2 );
		result[1] = executive::tex::sample< 1, int >( 
			texture, c0, c1, c2 );
		result[2] = executive::tex::sample< 2, int >( 
			texture, c0, c1, c2 );
		result[3] = executive::tex::sample< 3, int >( 
			texture, c0, c1, c2 );				
	}

	void __ocelot_tex_3d_uf( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, float c0, 
		float c1, float c2, float c3 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, unsigned int >( 
			texture, c0, c1, c2 );
		result[1] = executive::tex::sample< 1, unsigned int >( 
			texture, c0, c1, c2 );
		result[2] = executive::tex::sample< 2, unsigned int >( 
			texture, c0, c1, c2 );
		result[3] = executive::tex::sample< 3, unsigned int >( 
			texture, c0, c1, c2 );				
	}

	void __ocelot_tex_3d_su( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0, 
		unsigned int c1, unsigned int c2, unsigned int c3 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, int >( 
			texture, c0, c1, c2 );
		result[1] = executive::tex::sample< 1, int >( 
			texture, c0, c1, c2 );
		result[2] = executive::tex::sample< 2, int >( 
			texture, c0, c1, c2 );
		result[3] = executive::tex::sample< 3, int >( 
			texture, c0, c1, c2 );				
	}

	void __ocelot_tex_3d_ss( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0, 
		unsigned int c1, unsigned int c2, unsigned int c3 )
	{
		__ocelot_tex_3d_su( result, context, index, c0, c1, c2, c3 );
	}

	void __ocelot_tex_3d_uu( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0, 
		unsigned int c1, unsigned int c2, unsigned int c3 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, unsigned int >( 
			texture, c0, c1, c2 );
		result[1] = executive::tex::sample< 1, unsigned int >( 
			texture, c0, c1, c2 );
		result[2] = executive::tex::sample< 2, unsigned int >( 
			texture, c0, c1, c2 );
		result[3] = executive::tex::sample< 3, unsigned int >( 
			texture, c0, c1, c2 );				
	}

	void __ocelot_tex_3d_us( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0, 
		unsigned int c1, unsigned int c2, unsigned int c3 )
	{
		__ocelot_tex_3d_uu( result, context, index, c0, c1, c2, c3 );
	}

	void __ocelot_tex_2d_fu( float* result, executive::LLVMContext* context, 
		unsigned int index, unsigned int c0, unsigned int c1 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, float >( texture, c0, c1 );
		result[1] = executive::tex::sample< 1, float >( texture, c0, c1 );
		result[2] = executive::tex::sample< 2, float >( texture, c0, c1 );
		result[3] = executive::tex::sample< 3, float >( texture, c0, c1 );		
	}

	void __ocelot_tex_2d_fs( float* result, executive::LLVMContext* context, 
		unsigned int index, unsigned int c0, unsigned int c1 )
	{
		__ocelot_tex_2d_fu( result, context, index, c0, c1 );
	}

	void __ocelot_tex_2d_ff( float* result, executive::LLVMContext* context, 
		unsigned int index, float c0, float c1 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, float >( texture, c0, c1 );
		result[1] = executive::tex::sample< 1, float >( texture, c0, c1 );
		result[2] = executive::tex::sample< 2, float >( texture, c0, c1 );
		result[3] = executive::tex::sample< 3, float >( texture, c0, c1 );	
	}

	void __ocelot_tex_2d_sf( unsigned int* result, 
		executive::LLVMContext* context, 
		unsigned int index, float c0, float c1 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, int >( texture, c0, c1 );
		result[1] = executive::tex::sample< 1, int >( texture, c0, c1 );
		result[2] = executive::tex::sample< 2, int >( texture, c0, c1 );
		result[3] = executive::tex::sample< 3, int >( texture, c0, c1 );
	}

	void __ocelot_tex_2d_uf( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, 
		float c0, float c1 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, 
			unsigned int >( texture, c0, c1 );
		result[1] = executive::tex::sample< 1, 
			unsigned int >( texture, c0, c1 );
		result[2] = executive::tex::sample< 2, 
			unsigned int >( texture, c0, c1 );
		result[3] = executive::tex::sample< 3, 
			unsigned int >( texture, c0, c1 );						
	}

	void __ocelot_tex_2d_us( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0, 
		unsigned int c1 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, 
			unsigned int >( texture, c0, c1 );
		result[1] = executive::tex::sample< 1, 
			unsigned int >( texture, c0, c1 );
		result[2] = executive::tex::sample< 2, 
			unsigned int >( texture, c0, c1 );
		result[3] = executive::tex::sample< 3, 
			unsigned int >( texture, c0, c1 );						
	}

	void __ocelot_tex_2d_uu( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, 
		unsigned int c0, unsigned int c1 )
	{
		__ocelot_tex_2d_us( result, context, index, c0, c1 );
	}

	void __ocelot_tex_2d_su( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, 
		unsigned int c0, unsigned int c1 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, int >( texture, c0, c1 );
		result[1] = executive::tex::sample< 1, int >( texture, c0, c1 );
		result[2] = executive::tex::sample< 2, int >( texture, c0, c1 );
		result[3] = executive::tex::sample< 3, int >( texture, c0, c1 );	
	}

	void __ocelot_tex_2d_ss( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, 
		unsigned int c0, unsigned int c1 )
	{
		__ocelot_tex_2d_su( result, context, index, c0, c1 );
	}

	void __ocelot_tex_1d_fs( float* result, executive::LLVMContext* context, 
		unsigned int index, unsigned int c0 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, float >( texture, c0 );
		result[1] = executive::tex::sample< 1, float >( texture, c0 );
		result[2] = executive::tex::sample< 2, float >( texture, c0 );
		result[3] = executive::tex::sample< 3, float >( texture, c0 );	
	}

	void __ocelot_tex_1d_fu( float* result, executive::LLVMContext* context, 
		unsigned int index, unsigned int c0 )
	{
		__ocelot_tex_1d_fs( result, context, index, c0 );
	}

	void __ocelot_tex_1d_ff( float* result, executive::LLVMContext* context, 
		unsigned int index, float c0 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, float >( texture, c0 );
		result[1] = executive::tex::sample< 1, float >( texture, c0 );
		result[2] = executive::tex::sample< 2, float >( texture, c0 );
		result[3] = executive::tex::sample< 3, float >( texture, c0 );		
	}

	void __ocelot_tex_1d_sf( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, float c0 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, int >( texture, c0 );
		result[1] = executive::tex::sample< 1, int >( texture, c0 );
		result[2] = executive::tex::sample< 2, int >( texture, c0 );
		result[3] = executive::tex::sample< 3, int >( texture, c0 );	
	}

	void __ocelot_tex_1d_uf( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, float c0 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, unsigned int >( texture, c0 );
		result[1] = executive::tex::sample< 1, unsigned int >( texture, c0 );
		result[2] = executive::tex::sample< 2, unsigned int >( texture, c0 );
		result[3] = executive::tex::sample< 3, unsigned int >( texture, c0 );	
	}

	void __ocelot_tex_1d_ss( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, int >( texture, c0 );
		result[1] = executive::tex::sample< 1, int >( texture, c0 );
		result[2] = executive::tex::sample< 2, int >( texture, c0 );
		result[3] = executive::tex::sample< 3, int >( texture, c0 );	
	}

	void __ocelot_tex_1d_su( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0 )
	{
		__ocelot_tex_1d_ss( result, context, index, c0 );
	}

	void __ocelot_tex_1d_us( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0 )
	{
		executive::LLVMExecutableKernel::OpaqueState* state = 
			(executive::LLVMExecutableKernel::OpaqueState*) context->other;
		const ir::Texture& texture = state->textures[ index ];
		
		result[0] = executive::tex::sample< 0, unsigned int >( texture, c0 );
		result[1] = executive::tex::sample< 1, unsigned int >( texture, c0 );
		result[2] = executive::tex::sample< 2, unsigned int >( texture, c0 );
		result[3] = executive::tex::sample< 3, unsigned int >( texture, c0 );	
	}

	void __ocelot_tex_1d_uu( unsigned int* result, 
		executive::LLVMContext* context, unsigned int index, unsigned int c0 )
	{
		__ocelot_tex_1d_us( result, context, index, c0 );
	}

}

namespace executive
{

	LLVMExecutableKernel::LLVMState LLVMExecutableKernel::_state;

	LLVMExecutableKernel::LLVMState::LLVMState()		
	{
		jit = 0;
	}
	
	void LLVMExecutableKernel::LLVMState::initialize()
	{
		#ifdef HAVE_LLVM
		if( jit == 0 )
		{
			report( "Bringing the LLVM JIT-Compiler online." );

			module = new llvm::Module( "Ocelot-LLVM-JIT-Blank Module", 
				llvm::getGlobalContext() );
			assertM( module != 0, "Creating global module failed." );
		
			moduleProvider = new llvm::ExistingModuleProvider( module );
			assertM( moduleProvider != 0, 
				"Creating the global module provider failed.");
		
			llvm::InitializeNativeTarget();
		
			jit = llvm::EngineBuilder( moduleProvider ).create();
		
			assertM( jit != 0, "Creating the JIT failed.");
			report( " The JIT is alive." );
		}
		#endif
	}
	
	LLVMExecutableKernel::LLVMState::~LLVMState()
	{
		#ifdef HAVE_LLVM
		reportE( jit != 0, "Deleting the LLVM JIT-Compiler." );
		delete jit;
		#endif
	}
	
	LLVMExecutableKernel::OpaqueState::OpaqueState()
	{
		timer.start();
	}
	
	unsigned int LLVMExecutableKernel::_pad( size_t& size, 
		unsigned int alignment )
	{
		unsigned int padding = alignment - ( size % alignment );
		padding = ( alignment == padding ) ? 0 : padding;
		size += padding;
		return padding;
	}

	void LLVMExecutableKernel::_optimizePtx()
	{
		report( " Running PTX optimizer" );

		report( "  Building dataflow graph." );
		_ptx->dfg();

		analysis::ConvertPredicationToSelectPass pass1;
		report( "  Running convert predicate to conditional select" );
		
		pass1.initialize( *module );
		pass1.runOnKernel( *_ptx );
		pass1.finalize();
		
		report( "  Running remove barrier pass." );
		
		reportE( REPORT_ALL_PTX_SOURCE, "   Code before pass:\n" << *_ptx );

		analysis::RemoveBarrierPass pass2;
		
		pass2.initialize( *module );
		pass2.runOnKernel( *_ptx );
		pass2.finalize();
		
		_barrierSupport = pass2.barriers();
		_resumePoint = pass2.resume();

		reportE( REPORT_ALL_PTX_SOURCE, "   Code after pass:\n" << *_ptx );
	}

	void LLVMExecutableKernel::_translateKernel()
	{
		#ifdef HAVE_LLVM
		if( _module == 0 )
		{
			report( "Translating PTX kernel \"" << name << "\" to LLVM" );

			#if (PRINT_OPTIMIZED_CFG > 0) && (REPORT_BASE > 0)
			std::ofstream file((name + "_cfg_original.dot").c_str());
			_ptx->cfg()->write(file, _ptx->instructions);
			file.close();
			#endif
			
			_optimizePtx();
			_buildDebuggingInformation();
			_allocateMemory();

			translator::PTXToLLVMTranslator translator( _optimizationLevel );

			#if (PRINT_OPTIMIZED_CFG > 0) && (REPORT_BASE > 0)
			file.open((name + "_cfg_optimized.dot").c_str());
			_ptx->cfg()->write(file, _ptx->instructions);
			file.close();
			#endif
							
			report( " Running translator" );
			ir::LLVMKernel* llvmKernel = static_cast< 
				ir::LLVMKernel* >( translator.translate( _ptx ) );

			report( " Assembling llvm module" );
			llvmKernel->assemble();

			report( " Parsing llvm assembly." );
			llvm::SMDiagnostic error;
			_module = new llvm::Module( name, llvm::getGlobalContext() );
			_module = llvm::ParseAssemblyString( llvmKernel->code().c_str(), 
				_module, error, llvm::getGlobalContext() );
			
			if( _module == 0 )
			{
				report( "  Parsing kernel failed, dumping code:\n" 
					<< llvmKernel->numberedCode() );
				std::string m;
				llvm::raw_string_ostream message( m );
				message << "LLVM Parser failed: ";
				error.Print( name.c_str(), message );
				
				throw hydrazine::Exception( message.str() );
			}

			#if ( REPORT_ORIGINAL_LLVM_SOURCE > 0 ) && ( REPORT_BASE > 0 )
			std::string m;
			llvm::raw_string_ostream code( m );
			code << *_module;
			report( " The initial code is:\n" << m );
			#endif
						
			report( " Checking module for errors." );
			std::string verifyError;
			if( llvm::verifyModule( *_module, 
				llvm::ReturnStatusAction, &verifyError ) )
			{
				report( "  Checking kernel failed, dumping code:\n" 
					<< llvmKernel->numberedCode() );
				delete llvmKernel;

				throw hydrazine::Exception( "LLVM Verifier failed for kernel: " 
					+ name + " : \"" + verifyError + "\"" );
			}

			delete llvmKernel;
						
			report( " Successfully created LLVM Module from translated PTX." );
			
			_optimize();
			
		}
		#else
		assertM( false, "LLVM support not compiled into Ocelot." );
		#endif
	}

	void LLVMExecutableKernel::_optimize( )
	{
		#ifdef HAVE_LLVM
		report( "Optimizing LLVM Code" );
	
		_moduleProvider = new llvm::ExistingModuleProvider( _module );
		assertM( _moduleProvider != 0, "Creating the module provider failed.");
		
		_state.jit->addModuleProvider( _moduleProvider );
		
		llvm::Function* function = 
			_module->getFunction( "_Z_ocelotTranslated_" + name );

		assertM( function != 0, 
			"Could not find function _Z_ocelotTranslated_" + name );

		if( _optimizationLevel != translator::Translator::DebugOptimization )
		{
			llvm::FunctionPassManager manager( _moduleProvider );
		
			manager.add( new llvm::TargetData( *_state.jit->getTargetData() ) );
			manager.add( llvm::createInstructionCombiningPass() );
			manager.add( llvm::createReassociatePass() );
			manager.add( llvm::createGVNPass() );
			manager.add( llvm::createCFGSimplificationPass() );
				
			manager.run( *function );
		}
		
		#if ( REPORT_OPTIMIZED_LLVM_SOURCE > 0 ) && ( REPORT_BASE > 0 )
		std::string m;
		llvm::raw_string_ostream code( m );
		code << *_module;
		report( " The optimized code is:\n" << m );
		#endif
	
		// This insanity is needed to avoid a warning on g++
		union
		{
			void* one;
			Function two;
		} cast;
	
		updateGlobalMemory();
		cast.one = _state.jit->getPointerToFunction( function );
		_function = cast.two;
		
		report( " Successfully jit compiled the kernel." );
		#endif
	}

	void LLVMExecutableKernel::_launchCtaNoBarriers( )
	{
		char* localBase = _context.local;
		bool done = true;
		for( int z = 0; z < _context.ntid.z; ++z )
		{
			_context.tid.z = z;
			for( int y = 0; y < _context.ntid.y; ++y )
			{
				_context.tid.y = y;
				for( int x = 0; x < _context.ntid.x; ++x )
				{
					_context.tid.x = x;
					reportE( REPORT_INSIDE_TRANSLATED_CODE, 
						"  Launching thread ( x " << x << ", y " << y 
						<< ", z " << z << " )" );
					_context.local = localBase 
						+ _context.localSize * threadId();
					unsigned int resume = _function( &_context );
					done &= resume == 0;
					reportE( REPORT_INSIDE_TRANSLATED_CODE, 
						"   Thread blocked at " << resume );
				}
			}
		}	

		_context.local = localBase;		
		assertM( done, 
			"Not all threads finished in kernel with no context switches" );
	}
	
	void LLVMExecutableKernel::_launchCtaWithBarriers( )
	{
		char* localBase = _context.local;
		bool done = false;
		
		for( unsigned int i = 0; i < threads(); ++i )
		{
			(*(unsigned int*)(localBase 
				+ i * _context.localSize + _resumePointOffset)) = 0;
		}
		
		while( !done )
		{
			done = true;
			for( int z = 0; z < _context.ntid.z; ++z )
			{
				_context.tid.z = z;
				for( int y = 0; y < _context.ntid.y; ++y )
				{
					_context.tid.y = y;
					for( int x = 0; x < _context.ntid.x; ++x )
					{
						_context.tid.x = x;
						_context.local = localBase 
							+ _context.localSize * threadId();
						reportE( REPORT_INSIDE_TRANSLATED_CODE, 
							"  Launching thread ( x " << x << ", y " << y 
							<< ", z " << z << " )" << " at resume point " 
							<< *((unsigned int*)(_context.local 
							+ _resumePointOffset)) );
						unsigned int resume = _function( &_context );
						done &= resume == 0;
						*((unsigned int*)(_context.local 
							+ _resumePointOffset)) = resume;
						reportE( REPORT_INSIDE_TRANSLATED_CODE, 
							"   Thread blocked at " << resume );
					}
				}
			}
		}

		_context.local = localBase;		
	}
	
	void LLVMExecutableKernel::_allocateParameterMemory( )
	{
		report( "  Allocating parameter memory." );
				
		_context.parameterSize = 0;
		
		AllocationMap map;
		
		for( ParameterVector::iterator parameter = _ptx->parameters.begin(); 
			parameter != _ptx->parameters.end(); ++parameter )
		{
			_pad( _context.parameterSize, parameter->getAlignment() );
			
			report( "   Allocated parameter " << parameter->name << " from "
				<< _context.parameterSize << " to " 
				<< ( _context.parameterSize + parameter->getSize() ) );
			
			parameter->offset = _context.parameterSize;
			_context.parameterSize += parameter->getSize();
			
			map.insert( std::make_pair( parameter->name, parameter->offset ) );
		}
		
		report( "  Allocated " << _context.parameterSize 
			<< " for parameter memory." );
		
		_context.parameter = new char[ _context.parameterSize ];
	
		report( "  Determining offsets of operands that use parameters" );
	
		for( PTXInstructionVector::iterator 
			instruction = _ptx->instructions.begin(); 
			instruction != _ptx->instructions.end(); ++instruction )
		{
			ir::PTXOperand* operands[] = { &instruction->d, &instruction->a, 
				&instruction->b, &instruction->c };
		
			if( instruction->opcode == ir::PTXInstruction::Mov
				|| instruction->opcode == ir::PTXInstruction::Ld
				|| instruction->opcode == ir::PTXInstruction::St )
			{
				for( unsigned int i = 0; i != 4; ++i )
				{
					if( operands[ i ]->addressMode == ir::PTXOperand::Address )
					{
						AllocationMap::iterator 
							parameter = map.find( operands[ i ]->identifier );
						if( parameter != map.end() )
						{
							report( "   For instruction \"" 
								<< instruction->toString() << "\" mapping \"" 
								<< parameter->first << "\" to " 
								<< parameter->second );
							operands[ i ]->offset += parameter->second;
						}
					}
				}
			}
			
		}
	}

	void LLVMExecutableKernel::_allocateSharedMemory( )
	{
		typedef std::unordered_set< std::string > StringSet;
		typedef std::deque< ir::PTXOperand* > OperandVector;
		typedef std::unordered_map< std::string, 
			ir::Module::GlobalMap::const_iterator > GlobalMap;

		report( "  Allocating shared memory." );
	
		AllocationMap map;
		GlobalMap sharedGlobals;
		StringSet external;
		OperandVector externalOperands;

		unsigned int externalAlignment = 1;		
		_context.sharedSize = 0;
				
		assert( module != 0 ); 

		for( ir::Module::GlobalMap::const_iterator 
			global = module->globals.begin(); 
			global != module->globals.end(); ++global ) 
		{
			if( global->second.statement.directive 
				== ir::PTXStatement::Shared ) 
			{
				if( global->second.statement.attribute 
					== ir::PTXStatement::Extern )
				{
					report( "   Allocating global external shared variable " 
						<< global->second.statement.name );
					assertM( external.count( 
						global->second.statement.name ) == 0, 
						"External global " << global->second.statement.name 
						<< " more than once." );
					external.insert( global->second.statement.name );
					externalAlignment = std::max( externalAlignment, 
						(unsigned int) global->second.statement.alignment );
					externalAlignment = std::max( externalAlignment, 
						ir::PTXOperand::bytes( 
						global->second.statement.type ) );
				}
				else 
				{
					report( "   Allocating global shared variable " 
						<< global->second.statement.name );
					sharedGlobals.insert( std::make_pair( 
						global->second.statement.name, global ) );
				}
			}
		}

		LocalMap::const_iterator local = _ptx->locals.begin();
		for( ; local != _ptx->locals.end(); ++local )
		{
			if( local->second.space == ir::PTXInstruction::Shared )
			{
				if( local->second.attribute == ir::PTXStatement::Extern )
				{
					report( "    Found local external shared variable " 
						<< local->second.name );
					assert( external.count( local->second.name ) == 0 );
					external.insert( local->second.name );
					externalAlignment = std::max( externalAlignment, 
						( unsigned int ) local->second.alignment );
					externalAlignment = std::max( externalAlignment, 
						ir::PTXOperand::bytes( local->second.type ) );
				}
				else
				{
					report( "   Found local shared variable " 
						<< local->second.name << " of size " 
						<< local->second.getSize() );
					_pad( _context.sharedSize, local->second.alignment );
					map.insert( std::make_pair( local->second.name, 
						_context.sharedSize ) );
					_context.sharedSize += local->second.getSize();
				}
			}
		}
		
		for( PTXInstructionVector::iterator 
			instruction = _ptx->instructions.begin(); 
			instruction != _ptx->instructions.end(); ++instruction )
		{
			ir::PTXOperand* operands[] = { &instruction->d, &instruction->a, 
				&instruction->b, &instruction->c };
		
			if( instruction->opcode == ir::PTXInstruction::Mov
				|| instruction->opcode == ir::PTXInstruction::Ld
				|| instruction->opcode == ir::PTXInstruction::St )
			{
				for( unsigned int i = 0; i != 4; ++i )
				{
					if( operands[ i ]->addressMode == ir::PTXOperand::Address )
					{
						StringSet::iterator si = external.find(
							operands[ i ]->identifier );
						if( si != external.end() ) 
						{
							externalOperands.push_back( operands[ i ] );
							continue;
						}
						
						GlobalMap::iterator gi = sharedGlobals.find(
							operands[ i ]->identifier );
						if( gi != sharedGlobals.end() )
						{
							ir::Module::GlobalMap::const_iterator 
								it = gi->second;
							sharedGlobals.erase( gi );
							
							report( "   Found global shared variable " 
								<< it->second.statement.name );
							
							_pad( _context.sharedSize, 
								it->second.statement.alignment );
							
							map.insert( std::make_pair( 
								it->second.statement.name, 
								_context.sharedSize ) );
							_context.sharedSize += it->second.statement.bytes();
						}				
					
						AllocationMap::iterator mapping = map.find( 
							operands[ i ]->identifier );
						if( map.end() != mapping ) 
						{
							instruction->addressSpace 
								= ir::PTXInstruction::Shared;
							operands[ i ]->offset += mapping->second;
							report("   For instruction " 
								<< instruction->toString() 
								<< ", mapping shared label " << mapping->first 
								<< " to " << mapping->second );
						}
					}
				}
			}
		}

		_pad( _context.sharedSize, externalAlignment );

		report( "   Mapping external shared variables." );
		for( OperandVector::iterator operand = externalOperands.begin(); 
			operand != externalOperands.end(); ++operand ) 
		{
			report( "    Mapping external shared label " 
				<< (*operand)->identifier << " to " << _context.sharedSize );
			(*operand)->offset += _context.sharedSize;
		}
	
		report( "   Total shared memory size is " << _context.sharedSize 
			<< " declared plus " << _externalSharedSize << " external." );
		_context.shared = new char[ _context.sharedSize + _externalSharedSize ];

	}
	
	void LLVMExecutableKernel::_allocateGlobalMemory( )
	{
		report( " Allocating global memory" );
	}
	
	void LLVMExecutableKernel::_allocateLocalMemory( )
	{
		report( " Allocating local memory" );
		AllocationMap map;

		_context.localSize = 0;
		
		for( LocalMap::iterator local = _ptx->locals.begin(); 
			local != _ptx->locals.end(); ++local )
		{
			if( local->second.space == ir::PTXInstruction::Local )
			{
				report( "   Found local local variable " 
					<< local->second.name << " of size " 
					<< local->second.getSize() );
				_pad( _context.localSize, local->second.alignment );
				map.insert( std::make_pair( local->second.name,
					_context.localSize ) );
				_context.localSize += local->second.getSize();
			}
		}
		
		for( PTXInstructionVector::iterator 
			instruction = _ptx->instructions.begin(); 
			instruction != _ptx->instructions.end(); ++instruction )
		{
			ir::PTXOperand* operands[] = { &instruction->d, &instruction->a, 
				&instruction->b, &instruction->c };
		
			if( instruction->opcode == ir::PTXInstruction::Mov
				|| instruction->opcode == ir::PTXInstruction::Ld
				|| instruction->opcode == ir::PTXInstruction::St )
			{
				for( unsigned int i = 0; i != 4; ++i )
				{
					if( operands[ i ]->addressMode == ir::PTXOperand::Address )
					{
						AllocationMap::iterator mapping = map.find( 
							operands[ i ]->identifier );
						if( map.end() != mapping ) 
						{
							instruction->addressSpace 
								= ir::PTXInstruction::Local;
							operands[ i ]->offset += mapping->second;
							report("   For instruction " 
								<< instruction->toString() 
								<< ", mapping local label " << mapping->first 
								<< " to " << mapping->second );
						}
					}
				}
			}
		}	

		report( "   Total local memory size is " << _context.localSize 
			<< " for " << threads() << " threads." );

		_context.local = new char[ threads() * _context.localSize ];		

		if( _barrierSupport )
		{
			report( "   Adding barrier sync point mapping." );
			AllocationMap::iterator mapping = map.find( _resumePoint );
			assert( mapping != map.end() );
			report( "    Creating map from syncpoint variable " 
				<< _resumePoint << " to local memory offset " 
				<< mapping->second );
			_resumePointOffset = mapping->second;
			
		}
	}
	
	void LLVMExecutableKernel::_allocateConstantMemory( )
	{
		report( " Allocating Constant Memory" );
		_context.constantSize = 0;
		_constants.clear();

		for( ir::Module::GlobalMap::const_iterator 
			global = module->globals.begin(); 
			global != module->globals.end(); ++global ) 
		{
			if( global->second.statement.directive 
				== ir::PTXStatement::Const ) 
			{
				report( "   Found global constant variable " 
					<< global->second.statement.name << " of size " 
					<< global->second.statement.bytes() );
				_pad( _context.constantSize, 
					global->second.statement.alignment );
				_constants.insert( std::make_pair( 
					global->second.statement.name, _context.constantSize ) );
				_context.constantSize += global->second.statement.bytes();
			}
		}
		
		for( PTXInstructionVector::iterator 
			instruction = _ptx->instructions.begin(); 
			instruction != _ptx->instructions.end(); ++instruction )
		{
			ir::PTXOperand* operands[] = { &instruction->d, &instruction->a, 
				&instruction->b, &instruction->c };
		
			if( instruction->opcode == ir::PTXInstruction::Mov
				|| instruction->opcode == ir::PTXInstruction::Ld
				|| instruction->opcode == ir::PTXInstruction::St )
			{
				for( unsigned int i = 0; i != 4; ++i )
				{
					if( operands[ i ]->addressMode == ir::PTXOperand::Address )
					{
						AllocationMap::iterator mapping = _constants.find( 
							operands[ i ]->identifier );
						if( _constants.end() != mapping ) 
						{
							instruction->addressSpace 
								= ir::PTXInstruction::Const;
							operands[ i ]->offset += mapping->second;
							report("   For instruction " 
								<< instruction->toString() 
								<< ", mapping constant label " << mapping->first 
								<< " to " << mapping->second );
						}
					}
				}
			}
		}	

		report( "   Total constant memory size is " << _context.constantSize 
			<< "." );

		_context.constant = new char[ _context.constantSize ];
	}
	
	void LLVMExecutableKernel::_allocateTextureMemory( )
	{
		report( " Allocating Texture Memory" );
		_opaque.textures.clear();
		
		AllocationMap map;
		unsigned int index = 0;
		
		for( PTXInstructionVector::iterator 
			instruction = _ptx->instructions.begin(); 
			instruction != _ptx->instructions.end(); ++instruction )
		{
			if( instruction->opcode == ir::PTXInstruction::Tex )
			{
				ir::Module::TextureMap::const_iterator 
					texture = module->textures.find(
					instruction->a.identifier );
				assert( texture != module->textures.end() );
		
				AllocationMap::iterator 
					allocation = map.find( texture->first );
				if( allocation == map.end() )
				{
					report( "  Allocating texture " << texture->first 
						<< " to index " << index );
					allocation = map.insert( 
						std::make_pair( texture->first, index++ ) ).first;
					_opaque.textures.push_back( texture->second );
				}
				instruction->a.reg = allocation->second;
			}
		}
	}
	
	void LLVMExecutableKernel::_allocateMemory()
	{
		report( " Allocating Memory" );
		
		_allocateParameterMemory();
		_allocateSharedMemory();
		_allocateGlobalMemory();
		_allocateLocalMemory();
		_allocateConstantMemory();
		_allocateTextureMemory();
	}
	
	void LLVMExecutableKernel::_buildDebuggingInformation()
	{
		if( _optimizationLevel 
			!= translator::Translator::DebugOptimization ) return;
		
		report( "Building debug information." );
		
		ir::BasicBlock::Id id = 0;
		
		for( analysis::DataflowGraph::iterator block = _ptx->dfg()->begin(); 
			block != _ptx->dfg()->end(); ++block )
		{
			block->block()->id = id++;
			_opaque.blocks.insert( std::make_pair( block->id(), 
				block->block() ) );
		}
	}
	
	LLVMExecutableKernel::LLVMExecutableKernel( ir::Kernel& k, 
		const executive::Executive* c, 
		translator::Translator::OptimizationLevel l ) : 
		ExecutableKernel( k, c ), _module( 0 ), _moduleProvider( 0 ), 
		_externalSharedSize( 0 ), _optimizationLevel( l )
	{
		assertM( k.ISA == ir::Instruction::PTX, 
			"LLVMExecutable kernel must be constructed from a PTXKernel" );
		ISA = ir::Instruction::LLVM;
		
		_ptx = new ir::PTXKernel( static_cast< ir::PTXKernel& >( k ) );
		
		_context.shared = 0;
		_context.local = 0;
		_context.parameter = 0;
		_context.constant = 0;
		_context.nctaid.z = 1;
		
		_context.ntid.x = 0;
		_context.ntid.y = 0;
		_context.ntid.z = 0;
		
		_context.other = (char*) &_opaque;
		_opaque.instructions = &_ptx->instructions;
		
		_state.initialize();
	}
	
	LLVMExecutableKernel::~LLVMExecutableKernel()
	{	
		#ifdef HAVE_LLVM
		if( _moduleProvider != 0 )
		{
			_state.jit->deleteModuleProvider( _moduleProvider );
		}
		#endif
		delete[] _context.local;
		delete[] _context.constant;
		delete[] _context.parameter;
		delete[] _context.shared;
		delete _ptx;
	}

	void LLVMExecutableKernel::launchGrid( int x, int y )
	{	
		_translateKernel();
		report( "Launching kernel \"" << name << "\" on grid ( x = " 
			<< x << ", y = " << y << " )"  );
		
		_context.nctaid.x = x;
		_context.nctaid.y = y;
		if( _barrierSupport )
		{
			report( " With barrier support." );
			for( int j = 0; j < y; ++j )
			{
				for( int i = 0; i < x; ++i )
				{
					reportE( REPORT_INSIDE_TRANSLATED_CODE, 
						" Launching cta ( " << i << ", " << j << " )" );
					_context.ctaid.x = i;
					_context.ctaid.y = j;
					_launchCtaWithBarriers();
				}
			}
		}
		else
		{
			report( " Without barrier support." );
			for( int j = 0; j < y; ++j )
			{
				for( int i = 0; i < x; ++i )
				{
					reportE( REPORT_INSIDE_TRANSLATED_CODE, 
						" Launching cta ( " << i << ", " << j << " )" );
					_context.ctaid.x = i;
					_context.ctaid.y = j;
					_launchCtaNoBarriers();
				}
			}		
		}
		report( " Kernel \"" << name << "\" finished successfully"  );
	}
	
	void LLVMExecutableKernel::setKernelShape( int x, int y, int z )
	{
		report( "Setting CTA shape to ( x = " << x << ", y = " 
			<< y << ", z = " << z << " ) for kernel \"" << name << "\""  );
		unsigned int previous = threads();
	
		_context.ntid.x = x;
		_context.ntid.y = y;
		_context.ntid.z = z;
		
		if( previous != threads() )
		{
			if( _context.local != 0 )
			{
				report( " Reallocating local memory of " << _context.localSize 
					<< " bytes per thread ( " 
					<< ( threads() * _context.localSize ) << " total )" );
				delete[] _context.local;
				_context.local = new char[ threads() * _context.localSize ];
			}
		}
	}
	
	unsigned int LLVMExecutableKernel::threads() const
	{
		return _context.ntid.x * _context.ntid.y * _context.ntid.z;
	}
	
	unsigned int LLVMExecutableKernel::threadId() const
	{
		return _context.ntid.x * _context.ntid.y * _context.tid.z 
			+ _context.ntid.x * _context.tid.y + _context.tid.x;
	}

	unsigned int LLVMExecutableKernel::constantMemorySize() const
	{
		return _context.constantSize;
	}

	unsigned int LLVMExecutableKernel::sharedMemorySize() const
	{
		return _context.sharedSize;
	}
	
	unsigned int LLVMExecutableKernel::localMemorySize() const
	{
		return _context.localSize;
	}

	void LLVMExecutableKernel::externSharedMemory( unsigned int bytes )
	{
		_translateKernel();
		
		if( bytes != _externalSharedSize )
		{
			report( "Setting external shared memory to " << bytes 
				<< " total size is " << (bytes + _context.sharedSize) );
			delete[] _context.shared;
			_externalSharedSize = bytes;
			_context.shared = new char[ _externalSharedSize
				+ _context.sharedSize ];
		}
	}

	void LLVMExecutableKernel::updateParameterMemory()
	{
		_translateKernel();
	
		size_t size = 0;
		for( ParameterVector::iterator parameter = parameters.begin();
			parameter != parameters.end(); ++parameter ) {
			_pad( size, parameter->getAlignment() );
			for( ir::Parameter::ValueVector::iterator 
				value = parameter->arrayValues.begin(); 
				value != parameter->arrayValues.end(); ++value ) {
				assertM( size < _context.parameterSize, "Size " << size 
					<< " not less than allocated parameter size " 
					<< _context.parameterSize );
				memcpy( _context.parameter + size, &value->val_b16, 
					parameter->getElementSize() );
				size += parameter->getElementSize();
			}
		}
	}
	
	void LLVMExecutableKernel::updateGlobalMemory()
	{
		#ifdef HAVE_LLVM
		report( "Updating global memory." );
		_state.jit->clearAllGlobalMappings();
		for( ir::Module::GlobalMap::const_iterator 
			global = module->globals.begin(); 
			global != module->globals.end(); ++global ) 
		{
			switch( global->second.statement.directive ) 
			{
				case ir::PTXStatement::Global:
				{
					llvm::GlobalValue* 
						value = _module->getNamedValue( global->first );
					assertM( value != 0, "Global variable " << global->first 
						<< " not found in llvm module." );
					report( " Binding global variable " << global->first 
						<< " to " << (void*)global->second.pointer );
					_state.jit->addGlobalMapping( value, 
						global->second.pointer );
					break;
				}
				default:
				{
					break;
				}
			}
		}
		#endif
	}
	
	void LLVMExecutableKernel::updateConstantMemory()
	{
		report( "Updating constant memory." );
		
		for( AllocationMap::iterator constant = _constants.begin(); 
			constant != _constants.end(); ++constant ) 
		{
			report( " Updating constant variable " << constant->first );
			ir::Module::GlobalMap::const_iterator 
				global = module->globals.find( constant->first );
			assert( global != module->globals.end() );
			assert( global->second.statement.directive 
				== ir::PTXStatement::Const );
			assert( global->second.statement.bytes() 
				+ constant->second <= _context.constantSize );
			memcpy( _context.constant + constant->second, 
				global->second.pointer, global->second.statement.bytes() );
		}
	}
}

#endif
