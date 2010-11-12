// $Id: iq_router_split.cpp -1   $

/*
Copyright (c) 2007-2009, Trustees of The Leland Stanford Junior University
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this 
list of conditions and the following disclaimer in the documentation and/or 
other materials provided with the distribution.
Neither the name of the Stanford University nor the names of its contributors 
may be used to endorse or promote products derived from this software without 
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR 
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON 
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS 
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cassert>
#include <limits>

#include "globals.hpp"
#include "random_utils.hpp"
#include "buffer.hpp"
#include "iq_router_split.hpp"
#include "switch_monitor.hpp"
#include "buffer_monitor.hpp"

IQRouterSplit::IQRouterSplit( const Configuration& config,
		    Module *parent, const string & name, int id,
		    int inputs, int outputs )
  : IQRouterBase( config, parent, name, id, inputs, outputs )
{
  // check constraints
  if(_routing_delay != 0)
    Error("This router architecture requires lookahead routing!");
  
  // Allocate the allocators
  string alloc_type = config.GetStr( "sw_allocator" );
  string arb_type = config.GetStr( "sw_alloc_arb_type" );
  int iters = config.GetInt("sw_alloc_iters");
  if(iters == 0) iters = config.GetInt("alloc_iters");
  _sw_allocator = Allocator::NewAllocator( this, "sw_allocator",
					   alloc_type,
					   _inputs*_input_speedup, 
					   _outputs*_output_speedup,
					   iters, arb_type );
  
  _vc_rr_offset.resize(_inputs*_vcs);
  _sw_rr_offset.resize(_inputs*_input_speedup);
  for(int i = 0; i < _inputs*_input_speedup; ++i)
    _sw_rr_offset[i] = i % _input_speedup;

  _use_fast_path.resize(_inputs*_vcs, true);
}

IQRouterSplit::~IQRouterSplit( )
{
  delete _sw_allocator;
}

void IQRouterSplit::_Alloc( )
{
  bool watched = false;
  vector<int> fast_path_vcs(_inputs);
  _sw_allocator->Clear( );
  
  for(int input = 0; input < _inputs; ++input) {
    
    fast_path_vcs[input] = -1;
    
    for(int s = 0; s < _input_speedup; ++s) {
      int expanded_input  = input * _input_speedup + s;
      
      // Arbitrate (round-robin) between multiple requesting VCs at the same 
      // input (handles the case when multiple VC's are requesting the same 
      // output port)
      int vc = _sw_rr_offset[expanded_input];
      assert((vc % _input_speedup) == s);

      for(int v = 0; v < _vcs; ++v) {
	
	Buffer * cur_buf = _buf[input];
	
	VC::eVCState vc_state = cur_buf->GetState(vc);
	
	if(cur_buf->FrontFlit(vc) && cur_buf->FrontFlit(vc)->watch) 
	  *gWatchOut << GetSimTime() << " | " << FullName() << " | "
		      << "Saw flit " << cur_buf->FrontFlit(vc)->id
		      << " in slow path." << endl;
	
	if(!cur_buf->Empty(vc)) {
	  
	  Flit * f = cur_buf->FrontFlit(vc);
	  assert(f);
	  
	  if(((vc_state != VC::vc_alloc) && (vc_state != VC::active)) ||
	     (cur_buf->GetStateTime(vc) < _sw_alloc_delay)) {
	    if(f->watch)
	      *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			  << "VC " << vc << " at input " << input 
			  << " is not ready for slow-path allocation (flit: " << f->id 
			  << ", state: " << VC::VCSTATE[vc_state] 
			  << ", state time: " << cur_buf->GetStateTime(vc) 
			  << ")." << endl;
	    vc += _input_speedup;
	    if(vc >= _vcs) vc = s;
	    continue;
	  }	    
	  
	  if(f->watch)
	    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
			<< "VC " << vc << " at input " << input
			<< " is requesting slow-path allocation (flit: " << f->id 
			<< ", state: " << VC::VCSTATE[vc_state]
			<< ")." << endl;
	  
	  const OutputSet * route_set = cur_buf->GetRouteSet(vc);
	  
	  int output = _vc_rr_offset[input*_vcs+vc];
	  
	  for(int output_index = 0; output_index < _outputs; ++output_index) {
	    
	    // in active state, we only care about our assigned output port
	    if(vc_state == VC::active) {
	      output = cur_buf->GetOutputPort(vc);
	    }
	    
	    // When input_speedup > 1, the virtual channel buffers are 
	    // interleaved to create multiple input ports to the switch.
	    // Similarily, the output ports are interleaved based on their 
	    // originating input when output_speedup > 1.
	    
	    assert(expanded_input == input * _input_speedup + vc % _input_speedup);
	    int expanded_output = output * _output_speedup + input % _output_speedup;
	    
	    if((_switch_hold_in[expanded_input] == -1) && 
	       (_switch_hold_out[expanded_output] == -1)) {
	      
	      BufferState * dest_buf = _next_buf[output];
	      
	      bool do_request = false;
	      int in_priority = numeric_limits<int>::min();
	      
	      // check if any suitable VCs are available and determine the 
	      // highest priority for this port
	      int vc_cnt = route_set->NumVCs(output);
	      assert(!((vc_state == VC::active) && (vc_cnt == 0)));
	      
	      for(int vc_index = 0; vc_index < vc_cnt; ++vc_index) {
		int vc_prio;
		int out_vc = route_set->GetVC(output, vc_index, &vc_prio);
		
		if((vc_state == VC::vc_alloc) &&
		   !dest_buf->IsAvailableFor(out_vc)) {
		  if(f->watch)
		    *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
				<< "VC " << out_vc << " at output "
				<< output << " is busy." << endl;
		  continue;
		} else if((vc_state == VC::active) && 
			  (out_vc != cur_buf->GetOutputVC(vc))) {
		  continue;
		}
		
		if(dest_buf->IsFullFor(out_vc)) {
		  if(f->watch)
		    *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
				<< "VC " << out_vc << " at output "
				<< output << " has no buffers available." << endl;
		  continue;
		}
		
		if(f->watch)
		  *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			      << "VC " << out_vc << " at output "
			      << output << " is available." << endl;
		if(!do_request || (vc_prio > in_priority)) {
		  do_request = true;
		  in_priority = vc_prio;
		}
	      }
	      
	      if(do_request) {
		
		if(f->watch) {
		  *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			      << "VC " << vc << " at input "
			      << input << " requests output " << output 
			      << " (flit: " << f->id
			      << ", exp. input: " << expanded_input
			      << ", exp. output: " << expanded_output
			      << ")." << endl;
		  watched = true;
		}
		
		// We could have requested this same input-output pair in a 
		// previous iteration; only replace the previous request if the 
		// current request has a higher priority (this is default 
		// behavior of the allocators). Switch allocation priorities 
		// are strictly determined by the packet priorities.
		
		_sw_allocator->AddRequest(expanded_input, expanded_output, vc, 
					  in_priority, cur_buf->GetPriority(vc));
		
	      }
	    }

	    // in active state, we only care about our assigned output port
	    if(vc_state == VC::active) {
	      break;
	    }
	    
	    output = (output + 1) % _outputs;
	  }
	}
	vc += _input_speedup;
	if(vc >= _vcs) vc = s;
      }
    }
    
    // dub: handle fast-path flits separately so we know all switch requests 
    // from other VCs that are on the regular path have been issued already
    for(int vc = 0; vc < _vcs; vc++) {
      
      if(_use_fast_path[input*_vcs+vc]) {
	
	Buffer * cur_buf = _buf[input];
	
	if(cur_buf->Empty(vc)) {
	  continue;
	}
	
	Flit * f = cur_buf->FrontFlit(vc);
	assert(f);
	
	if(f->watch)
	  *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
		      << "Saw flit " << f->id
		      << " in fast path." << endl;
	
	VC::eVCState vc_state = cur_buf->GetState(vc);
	
	if(((vc_state != VC::vc_alloc) && (vc_state != VC::active)) ||
	   (cur_buf->GetStateTime(vc) < _sw_alloc_delay)) {
	  if(f->watch)
	    *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			<< "VC " << vc << " at input " << input 
			<< " is not ready for fast-path allocation (flit: " << f->id 
			<< ", state: " << VC::VCSTATE[vc_state] 
			<< ", state time: " << cur_buf->GetStateTime(vc) 
			<< ")." << endl;
	  continue;
	}	    
	
	if(f->watch)
	  *gWatchOut << GetSimTime() << " | " << FullName() << " | "
		      << "VC " << vc << " at input " << input
		      << " is requesting fast-path allocation (flit: " << f->id 
		      << ", state: " << VC::VCSTATE[vc_state]
		      << ")." << endl;
	
	if(fast_path_vcs[input] >= 0)
	  cout << "XXX" << endl
	       << FullName() << endl
	       << "VC: " << vc << ", input: " << input << ", flit: " << f->id
	       << ", fast VC: " << fast_path_vcs[input] << ", fast flit: "
	       << cur_buf->FrontFlit(fast_path_vcs[input])->id << endl
	       << "XXX" << endl;
	assert(fast_path_vcs[input] < 0);
	
	fast_path_vcs[input] = vc;
	
	const OutputSet * route_set = cur_buf->GetRouteSet(vc);
	int expanded_input = input * _input_speedup + vc % _input_speedup;
	
	for(int output = 0; output < _outputs; ++output) {
	  
	  // dub: if we're done with VC allocation, we already know our output
	  if(vc_state == VC::active) {
	    output = cur_buf->GetOutputPort(vc);
	  }
	  
	  BufferState * dest_buf = _next_buf[output];
	  
	  int expanded_output = 
	    output * _output_speedup + input % _output_speedup;
	  
	  if(_sw_allocator->ReadRequest(expanded_input, expanded_output) >= 0) {
	    if(f->watch)
	      *gWatchOut << GetSimTime() << " | " << FullName() << " | "
			  << "Crossbar slot is already in use by slow path "
			  << "(exp. input: " << expanded_input
			  << ", exp. output: " << expanded_output
			  << ")." << endl;
	    if(vc_state == VC::active) {
	      break;
	    } else {
	      continue;
	    }
	  }
	  
	  bool do_request = false;
	  int in_priority = numeric_limits<int>::min();
	  
	  int vc_cnt = route_set->NumVCs(output);
	  assert((vc_state != VC::active) || (vc_cnt > 0));
	  
	  for(int vc_index = 0; vc_index < vc_cnt; ++vc_index) {
	    int vc_prio;
	    int out_vc = route_set->GetVC(output, vc_index, &vc_prio);
	    
	    if((vc_state == VC::vc_alloc) && 
	       !dest_buf->IsAvailableFor(out_vc)) {
	      if(f->watch)
		*gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			    << "VC " << out_vc << " at output "
			    << output << " is busy." << endl;
	      continue;
	    } else if((vc_state == VC::active) && 
		      (out_vc != cur_buf->GetOutputVC(vc))) {
	      continue;
	    }
	    
	    if(dest_buf->IsFullFor(out_vc)) {
	      if(f->watch)
		*gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			    << "VC " << out_vc << " at output "
			    << output << " has no buffers available." << endl;
	      continue;
	    }
	    
	    if(f->watch)
	      *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			  << "VC " << out_vc << " at output "
			  << output << " is available." << endl;
	    if(!do_request || (vc_prio > in_priority)) {
	      do_request = true;
	      in_priority = vc_prio;
	    }	    
	  }
	  
	  if(do_request) {
	    
	    if(f->watch)
	      *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			  << "VC " << vc << " at input "
			  << input << " requests output " << output 
			  << " (flit: " << f->id
			  << ", exp. input: " << expanded_input
			  << ", exp. output: " << expanded_output
			  << ")." << endl;
	    
	    // We could have requested this same input-output pair in a 
	    // previous iteration; only replace the previous request if the 
	    // current request has a higher priority (this is default 
	    // behavior of the allocators). Switch allocation priorities 
	    // are strictly determined by the packet priorities.
	    
	    _sw_allocator->AddRequest(expanded_input, expanded_output, vc, 
				      in_priority, cur_buf->GetPriority(vc));
	    
	  }
	  
	  if(vc_state == VC::active) {
	    break;
	  }
	}
      }
    }
  }
  
  if(watched) {
    *gWatchOut << GetSimTime() << " | " << _sw_allocator->FullName() << " | ";
    _sw_allocator->PrintRequests(gWatchOut);
  }
  
  _sw_allocator->Allocate();
  
  if(watched) {
    *gWatchOut << GetSimTime() << " | " << _sw_allocator->FullName() << " | "
		<< "Grants = [ ";
    for(int input = 0; input < _inputs; ++input)
      for(int s = 0; s < _input_speedup; ++s) {
	int expanded_input = input * _input_speedup + s;
	int expanded_output = _sw_allocator->OutputAssigned(expanded_input);
	if(expanded_output > -1) {
	  int output = expanded_output / _output_speedup;
	  int vc = _sw_allocator->ReadRequest(expanded_input, expanded_output);
	  *gWatchOut << input << " -> " << output << " (vc:" << vc << ")  ";
	}
      }
    *gWatchOut << "]." << endl;
  }
  
  // Winning flits cross the switch

  _crossbar_pipe->WriteAll(NULL);

  //////////////////////////////
  // Switch Power Modelling
  //  - Record Total Cycles
  //
  _switchMonitor->cycle() ;

  for(int input = 0; input < _inputs; ++input) {
    
    Credit * c = NULL;
    
    for(int s = 0; s < _input_speedup; ++s) {
      
      int expanded_input = input * _input_speedup + s;
      int expanded_output;
      Buffer * cur_buf = _buf[input];
      int vc;
      int fvc = fast_path_vcs[input];
      
      if(_switch_hold_in[expanded_input] != -1) {
	assert(_switch_hold_in[expanded_input] >= 0);
	expanded_output = _switch_hold_in[expanded_input];
	vc = _switch_hold_vc[expanded_input];
	
	if(cur_buf->Empty(vc)) { // Cancel held match if VC is empty
	  expanded_output = -1;
	}
      } else {
	expanded_output = _sw_allocator->OutputAssigned(expanded_input);
	if(expanded_output >= 0) {
	  vc = _sw_allocator->ReadRequest(expanded_input, expanded_output);
	} else {
	  vc = -1;
	}
      }
      
      if(expanded_output >= 0) {
	
	int output = expanded_output / _output_speedup;
	
	Flit * f = cur_buf->FrontFlit(vc);
	assert(f);
	
	if(vc == fvc) {
	  if(f->watch)
	    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
			<< "Fast-path allocation successful for VC " << vc
			<< " at input " << input << " (flit: " << f->id
			<< ")." << endl;
	} else {
	  if(f->watch)
	    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
			<< "Slow-path allocation successful for VC " << vc
			<< " at input " << input << " (flit: " << f->id
			<< ")." << endl;
	  if(fvc >= 0) {
	    assert(_use_fast_path[input*_vcs+fvc]);
	    assert(cur_buf->FrontFlit(fvc));
	    if(cur_buf->FrontFlit(fvc)->watch)
	      cout << GetSimTime() << " | " << FullName() << " | "
		   << "Disabling fast-path allocation for VC " << fvc
		   << " at input " << input << "." << endl;
	    _use_fast_path[input*_vcs+fvc] = false;
	  }
	}
	
	BufferState * dest_buf = _next_buf[output];
	
	if(cur_buf->GetState(vc) == VC::vc_alloc) {
	  
	  const OutputSet * route_set = cur_buf->GetRouteSet(vc);
	  int sel_prio = -1;
	  int sel_vc = -1;
	  int vc_cnt = route_set->NumVCs(output);
	  
	  for(int vc_index = 0; vc_index < vc_cnt; ++vc_index) {
	    int out_prio;
	    int out_vc = route_set->GetVC(output, vc_index, &out_prio);
	    if(!dest_buf->IsAvailableFor(out_vc)) {
	      if(f->watch)
		*gWatchOut << GetSimTime() << " | " << FullName() << " | "
			   << "VC " << out_vc << " at output "
			   << output << " is busy." << endl;
	      continue;
	    }
	    if(dest_buf->IsFullFor(out_vc)) {
	      if(f->watch)
		*gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			   << "VC " << out_vc << " at output "
			   << output << " has no buffers available." << endl;
	      continue;
	    }
	    
	    if(f->watch)
	      *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			 << "VC " << out_vc << " at output "
			 << output << " is available." << endl;
	    if(out_prio > sel_prio) {
	      sel_vc = out_vc;
	      sel_prio = out_prio;
	    }
	  }
	  
	  if(sel_vc < 0) {
	    cout << "XXX" << endl
		 << "Flit " << f->id
		 << ", VC " << vc
		 << ", input " << input << ":" << endl
		 << "None of " << vc_cnt << " VCs at output " << output 
		 << " were suitable and available." << endl 
		 << "XXX" << endl;
	  }
	  // we should only get to this point if some VC requested 
	  // allocation
	  assert(sel_vc > -1);
	  
	  cur_buf->SetState(vc, VC::active);
	  cur_buf->SetOutput(vc, output, sel_vc);
	  dest_buf->TakeBuffer(sel_vc);
	  
	  _vc_rr_offset[input*_vcs+vc] = (output + 1) % _outputs;
	  
	  if(f->watch)
	    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
		       << "VC " << sel_vc << " at output " << output
		       << " granted to VC " << vc << " at input " << input
		       << " (flit: " << f->id << ")." << endl;
	}
	  
	if(cur_buf->GetState(vc) == VC::active) {
	  
	  if(_hold_switch_for_packet) {
	    _switch_hold_in[expanded_input] = expanded_output;
	    _switch_hold_vc[expanded_input] = vc;
	    _switch_hold_out[expanded_output] = expanded_input;
	  }
	  
	  //assert(cur_vc->GetState() == VC::active);
	  assert(!cur_buf->Empty(vc));
	  assert(cur_buf->GetOutputPort(vc) == output);
	  
	  dest_buf = _next_buf[output];
	  
	  assert(!dest_buf->IsFullFor(cur_buf->GetOutputVC(vc)));
	  
	  // Forward flit to crossbar and send credit back
	  f = cur_buf->RemoveFlit(vc);
	  
	  if(f->watch)
	    *gWatchOut << GetSimTime() << " | " << FullName() << " | " 
			<< "Output " << output
			<< " granted to VC " << vc << " at input " << input
			<< " (flit: " << f->id
			<< ", exp. input: " << expanded_input
			<< ", exp. output: " << expanded_output
			<< ")." << endl;
	  
	  f->hops++;
	  
	  //
	  // Switch Power Modelling
	  //
	  _switchMonitor->traversal(input, output, f);
	  _bufferMonitor->read(input, f);
	  
	  if(f->watch)
	    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
			<< "Forwarding flit " << f->id << " through crossbar "
			<< "(exp. input: " << expanded_input
			<< ", exp. output: " << expanded_output
			<< ")." << endl;
	  
	  if (c == NULL) {
	    c = _NewCredit(_vcs);
	  }
	  
	  assert(vc == f->vc);
	  
	  c->vc[c->vc_cnt] = vc;
	  c->vc_cnt++;
	  c->dest_router = f->from_router;
	  f->vc = cur_buf->GetOutputVC(vc);
	  dest_buf->SendingFlit(f);
	  
	  _crossbar_pipe->Write(f, expanded_output);
	  
	  if(f->tail) {
	    cur_buf->SetState(vc, VC::idle);
	    if(!cur_buf->Empty(vc)) {
	      _queuing_vcs.push(make_pair(input, vc));
	    }
	    _switch_hold_in[expanded_input] = -1;
	    _switch_hold_vc[expanded_input] = -1;
	    _switch_hold_out[expanded_output] = -1;
	  }	  
	  
	  if(!_use_fast_path[input*_vcs+vc]) {
	    int next_offset = vc + _input_speedup;
	    _sw_rr_offset[expanded_input] = 
	      (next_offset < _vcs) ? next_offset : s;
	  }
	  
	  if(cur_buf->Empty(vc) && !_use_fast_path[input*_vcs+vc]) {
	    if(f->watch)
	      *gWatchOut << GetSimTime() << " | " << FullName() << " | "
			  << "Enabling fast-path allocation for VC " << vc
			  << " at input " << input << "." << endl;
	    _use_fast_path[input*_vcs+vc] = true;
	  }
	  
	} 
      } else if((fvc >= 0) && ((fvc % _input_speedup) == s)) {
	
	assert(_use_fast_path[input*_vcs+fvc]);
	
	Flit * f = cur_buf->FrontFlit(fvc);
	assert(f);
	
	if(f->watch)
	  *gWatchOut << GetSimTime() << " | " << FullName() << " | "
		      << "Disabling fast-path allocation for VC " << fvc
		      << " at input " << input << "." << endl;
	_use_fast_path[input*_vcs+fvc] = false;
      }
    }    
    _credit_pipe->Write(c, input);
  }
}
