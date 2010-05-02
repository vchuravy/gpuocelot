/*! \file PTXOptimzer.cpp
	\date Thursday December 31, 2009
	\author Gregory Diamos <gregory.diamos@gatech.edu>
	\brief The source file for the Ocelot PTX optimizer
*/

#ifndef PTX_OPTIMIZER_CPP_INCLUDED
#define PTX_OPTIMIZER_CPP_INCLUDED

#include <ocelot/analysis/interface/PTXOptimizer.h>
#include <ocelot/analysis/interface/LinearScanRegisterAllocationPass.h>
#include <ocelot/analysis/interface/RemoveBarrierPass.h>
#include <ocelot/analysis/interface/ConvertPredicationToSelectPass.h>
#include <ocelot/ir/interface/Module.h>

#include <hydrazine/implementation/ArgumentParser.h>
#include <hydrazine/implementation/Exception.h>
#include <hydrazine/implementation/string.h>
#include <hydrazine/implementation/debug.h>

#ifdef REPORT_BASE
#undef REPORT_BASE
#endif

#define REPORT_BASE 0

#include <fstream>

namespace analysis
{
	PTXOptimizer::PTXOptimizer() : registerAllocationType( 
		InvalidRegisterAllocationType ), passes( 0 )
	{
	
	}

	void PTXOptimizer::optimize()
	{
		typedef std::vector< KernelPass* > PassVector;
		
		report("Running PTX to PTX Optimizer.");
		
		PassVector ssaPasses;
		PassVector noSsaPasses;
		
		if( registerAllocationType == LinearScan )
		{
			KernelPass* pass = new analysis::LinearScanRegisterAllocationPass( 
				registerCount );
			if( pass->ssa )
			{
				ssaPasses.push_back( pass );
			}
			else
			{
				noSsaPasses.push_back( pass );
			}
		}
		
		if( passes & RemoveBarriers )
		{
			KernelPass* pass = new analysis::RemoveBarrierPass;
			if( pass->ssa )
			{
				ssaPasses.push_back( pass );
			}
			else
			{
				noSsaPasses.push_back( pass );
			}			
		}

		if( passes & ReverseIfConversion )
		{
			KernelPass* pass = new analysis::ConvertPredicationToSelectPass;
			if( pass->ssa )
			{
				ssaPasses.push_back( pass );
			}
			else
			{
				noSsaPasses.push_back( pass );
			}			
		}
		
		if( input.empty() )
		{
			std::cout << "No input file name given.  Bailing out." << std::endl;
			return;
		}
		
		report(" Loading module '" << input << "'");
		ir::Module module( input );

		report(" Running register allocation.");
		module.createDataStructures();
		
		report(" Running passes that do not require SSA form.");
		for( PassVector::iterator pass = noSsaPasses.begin(); 
			pass != noSsaPasses.end(); ++pass)
		{
			report("  Running pass '" << (*pass)->toString() << "'" );
			(*pass)->initialize( module );
			for( ir::Module::KernelMap::iterator 
				kernel = module.kernels.begin(); 
				kernel != module.kernels.end(); ++kernel )
			{
				(*pass)->runOnKernel( *(kernel->second) );
			}
			(*pass)->finalize();
			delete *pass;
		}
		
		report(" Converting to SSA form.");
		for( ir::Module::KernelMap::iterator 
			kernel = module.kernels.begin(); 
			kernel != module.kernels.end(); ++kernel )
		{
			(kernel->second)->dfg()->toSsa();
		}
	
		report(" Running passes that require SSA form.");
		for( PassVector::iterator pass = ssaPasses.begin(); 
			pass != ssaPasses.end(); ++pass)
		{
			report("  Running pass '" << (*pass)->toString() << "'" );
			(*pass)->initialize( module );
			for( ir::Module::KernelMap::iterator 
				kernel = module.kernels.begin(); 
				kernel != module.kernels.end(); ++kernel )
			{
				(*pass)->runOnKernel( *(kernel->second) );
			}
			(*pass)->finalize();
			delete *pass;
		}

		for( ir::Module::KernelMap::iterator kernel = module.kernels.begin(); 
			kernel != module.kernels.end(); ++kernel )
		{
			(kernel->second)->dfg()->fromSsa();
		}
		
		std::ofstream out( output.c_str() );
		
		if( !out.is_open() )
		{
			throw hydrazine::Exception( "Could not open output file " 
				+ output + " for writing." );
		}
		
		module.writeIR( out );
	}
}

static int parsePassTypes( const std::string& passList )
{
	int types = analysis::PTXOptimizer::InvalidPassType;
	
	report("Checking for pass types.");
	hydrazine::StringVector passes = hydrazine::split( passList, "," );
	for( hydrazine::StringVector::iterator pass = passes.begin(); 
		pass != passes.end(); ++pass )
	{
		*pass = hydrazine::strip( *pass, " " );
		report( " Checking option '" << *pass << "'" );
		if( *pass == "remove-barriers" )
		{
			report( "  Matched remove-barriers." );
			types |= analysis::PTXOptimizer::RemoveBarriers;
		}
		else if( *pass == "reverse-if-conversion" )
		{
			report( "  Matched reverse-if-conversion." );
			types |= analysis::PTXOptimizer::ReverseIfConversion;
		}
		else
		{
			std::cout << "==Ocelot== Warning: Unknown pass name - '" << *pass 
				<< "'\n";
		}
	}
	return types;
}

int main( int argc, char** argv )
{
	hydrazine::ArgumentParser parser( argc, argv );
	parser.description( "The Ocelot PTX to PTX optimizer." );
	analysis::PTXOptimizer optimizer;
	std::string allocator;
	std::string passes;
	
	parser.parse( "-i", "--input", optimizer.input, "",
		"The ptx file to be optimized." );
	parser.parse( "-o", "--output", optimizer.output, 
		"_optimized_" + optimizer.input, "The resulting optimized file." );
	parser.parse( "-a", "--allocator", allocator, "none",
		"The type of register allocator to use (linearscan)." );
	parser.parse( "-r", "--max-registers", optimizer.registerCount, 32,
		"The number of registers available for allocation." );
	parser.parse( "-p", "--passes", passes, "", 
		"A list of optimization passes (remove-barriers, " 
		+ std::string( "reverse-if-conversion)") );
	parser.parse();

	if( allocator == "linearscan" )
	{
		optimizer.registerAllocationType = analysis::PTXOptimizer::LinearScan;
	}
	
	optimizer.passes = parsePassTypes( passes );
	
	optimizer.optimize();

	return 0;
}

#endif
