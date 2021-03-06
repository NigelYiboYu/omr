/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2000, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 ******************************************************************************/

#include "env/ExceptionTable.hpp"

#include <stdint.h>                 // for uint32_t, int32_t
#include "compile/Compilation.hpp"  // for Compilation
#include "env/TRMemory.hpp"
#include "env/jittypes.h"           // for TR_ByteCodeInfo
#include "il/Block.hpp"             // for Block, etc
#include "il/Node.hpp"              // for Node
#include "il/TreeTop.hpp"           // for TreeTop
#include "il/TreeTop_inlines.hpp"   // for TreeTop::getNode, etc
#include "infra/Array.hpp"          // for TR_Array
#include "infra/Assert.hpp"         // for TR_ASSERT
#include "infra/List.hpp"           // for List, ListIterator
#include "infra/TRCfgEdge.hpp"      // for CFGEdge

class TR_ResolvedMethod;

TR_ExceptionTableEntryIterator::TR_ExceptionTableEntryIterator(TR::Compilation *comp)
   : _compilation(comp)
   {
   int32_t i = comp->getMaxInlineDepth() + 1;
   _tableEntries = (TR_Array<List<TR_ExceptionTableEntry> > *)comp->trMemory()->allocateHeapMemory(sizeof(TR_Array<List<TR_ExceptionTableEntry> >)*i);
   for (int32_t j = 0; j < i; ++j)
      _tableEntries[j].init(comp->trMemory());

   // walk the trees looking for catch blocks
   //
   TR::Block * catchBlock;
   TR::TreeTop * tt = comp->getStartTree();
   for (; tt; tt = catchBlock->getExit()->getNextTreeTop())
      {
      catchBlock = tt->getNode()->getBlock();

      // Skip OSR blocks
      if (catchBlock->isOSRCodeBlock() || catchBlock->isOSRCatchBlock())
         continue;

      if (!catchBlock->getExceptionPredecessors().empty())
         {
         List<TR_ExceptionTableEntry> & tableEntries =
            _tableEntries[catchBlock->getInlineDepth()][catchBlock->getHandlerIndex()];

         tableEntries._trMemory = comp->trMemory();

         uint32_t catchType = catchBlock->getCatchType();
         TR_ResolvedMethod * method = catchBlock->getOwningMethod();

         // create exception ranges from the list of exception predecessors
         //
         TR::list<TR::CFGEdge*> & exceptionPredecessors = catchBlock->getExceptionPredecessors();
         while (!exceptionPredecessors.empty())
            {
            TR::CFGEdge * e = exceptionPredecessors.front();
            exceptionPredecessors.pop_front();
            TR::Block * first, * last;
            first = last = toBlock(e->getFrom());
            addSnippetRanges(tableEntries, first, catchBlock, catchType, method, comp);

            // extend the range backwards
            //
            for (;;)
               {
               TR::TreeTop * tt = first->getEntry()->getPrevTreeTop();
               if (!tt) break;
               bool found = false;
               TR_ASSERT(tt->getNode(), "a treetop doesn't have a node ????");
               TR::Block * prevBlock = tt->getNode()->getBlock();
               for (auto e = exceptionPredecessors.begin(); e != exceptionPredecessors.end(); ++e)
                  if (toBlock((*e)->getFrom()) == prevBlock)
                     { exceptionPredecessors.erase(e); found = true; break; }
               if (!found) break;
               first = prevBlock;
               addSnippetRanges(tableEntries, prevBlock, catchBlock, catchType, method, comp);
               }

            // extend the range forwards
            //
            for (;;)
               {
               TR::TreeTop * tt = last->getExit()->getNextTreeTop();
               if (!tt) break;
               bool found = false;
               TR::Block * nextBlock = tt->getNode()->getBlock();
               for (auto e = exceptionPredecessors.begin(); e != exceptionPredecessors.end(); ++e)
                  if (toBlock((*e)->getFrom()) == nextBlock)
                     { exceptionPredecessors.erase(e); found = true; break; }
               if (!found) break;
               last = nextBlock;
               addSnippetRanges(tableEntries, nextBlock, catchBlock, catchType, method, comp);
               }

            TR_ExceptionTableEntry * ete = new (comp->trHeapMemory()) TR_ExceptionTableEntry;
            ete->_instructionStartPC = first->getInstructionBoundaries()._startPC;
            ete->_instructionEndPC = last->getInstructionBoundaries()._endPC;
            ete->_instructionHandlerPC = catchBlock->getInstructionBoundaries()._startPC;
            ete->_catchType = catchType;
            ete->_method = method;

            ete->_byteCodeInfo = catchBlock->getByteCodeInfo();
            tableEntries.add(ete);
            }
         }
      }
   }

void
TR_ExceptionTableEntryIterator::addSnippetRanges(
   List<TR_ExceptionTableEntry> & tableEntries, TR::Block * snippetBlock, TR::Block * catchBlock, uint32_t catchType,
   TR_ResolvedMethod * method, TR::Compilation *comp)
   {
   TR::Block::InstructionBoundaries * ib = snippetBlock->getFirstSnippetBoundaries();
   for (; ib; ib = ib->getNext())
      {
      TR_ExceptionTableEntry * ete = new (comp->trHeapMemory()) TR_ExceptionTableEntry;
      ete->_instructionStartPC = ib->_startPC;
      ete->_instructionEndPC = ib->_endPC;
      ete->_instructionHandlerPC = catchBlock->getInstructionBoundaries()._startPC;
      ete->_catchType = catchType;
      ete->_method = method;

      ete->_byteCodeInfo = catchBlock->getByteCodeInfo();
      tableEntries.add(ete);
      }
   }

TR_ExceptionTableEntry *
TR_ExceptionTableEntryIterator::getFirst()
   {
   _inlineDepth = _compilation->getMaxInlineDepth();
   if (_inlineDepth >= 0)
      {
      _handlerIndex = 0;
      _entryIterator.set(&_tableEntries[_inlineDepth][_handlerIndex]);
      }
   return getCurrent();
   }

TR_ExceptionTableEntry *
TR_ExceptionTableEntryIterator::getNext()
   {
   _entryIterator.getNext();
   return getCurrent();
   }

TR_ExceptionTableEntry *
TR_ExceptionTableEntryIterator::getCurrent()
   {
   if (_inlineDepth < 0) return 0;

   while (_entryIterator.getCurrent() == 0)
      {
      if (++_handlerIndex >= _tableEntries[_inlineDepth].size())
         {
         if (--_inlineDepth < 0) return 0;
         _handlerIndex = 0;
         }
      _entryIterator.set(&_tableEntries[_inlineDepth][_handlerIndex]);
      }
   return _entryIterator.getCurrent();
   }

uint32_t
TR_ExceptionTableEntryIterator::size()
   {
   uint32_t i = 0;
   for (TR_ExceptionTableEntry * e = getFirst(); e; e = getNext())
      ++i;
   return i;
   }
