// -------------------------------------------------------------------------------------------------
//  File LLNode.hpp
//
//  Copyright (c) 2020-2024 DexLogic, Dirk Apitz
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//  SOFTWARE.
//
// -------------------------------------------------------------------------------------------------
//  Change History:
//
//  01/2020 Dirk Apitz, created
//  01/2024 Dirk Apitz, modifications and integration into OpenIDN
// -------------------------------------------------------------------------------------------------


#ifndef LLNODE_HPP
#define LLNODE_HPP



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

template <class T> class LLNode
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    LLNode<T> *next;
    LLNode<T> **ref;


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    LLNode()
    {
        next = (LLNode<T> *)0;
        ref = (LLNode<T> **)0;
    }

    ~LLNode()
    {
    }

    void linkin(LLNode<T> **ppNode)
    {
        // Note/Assertion: next = (PNode<T> *)0), ref = (PNode<T> **)0

        next = *ppNode;
        if(next != (LLNode<T> *)0) next->ref = &next;

        *ppNode = this;
        ref = ppNode;
    }

    void linkinLast(LLNode<T> **ppNode)
    {
        // Note/Assertion: next = (PNode<T> *)0), ref = (PNode<T> **)0

        while(*ppNode != (LLNode<T> *)0) ppNode = &((*ppNode)->next);

        // Note/Assertion: *ppNode = (DLNode<T> *)0

        *ppNode = this;
        ref = ppNode;
    }

    void linkout()
    {
        if(next) next->ref = ref;
        if(ref != (LLNode<T> **)0) *ref = next;

        next = (LLNode<T> *)0;
        ref = (LLNode<T> **)0;
    }

    LLNode<T> *getNextNode()
    {
        return next;
    }
};


#endif

