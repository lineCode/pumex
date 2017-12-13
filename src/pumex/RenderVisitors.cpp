//
// Copyright(c) 2017 Paweł Księżopolski ( pumexx )
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
#include <pumex/RenderVisitors.h>
#include <pumex/AssetBufferNode.h>

using namespace pumex;

ValidateGPUVisitor::ValidateGPUVisitor(Surface* s)
  : NodeVisitor{ AllChildren }, renderContext(s)
{
}


void ValidateGPUVisitor::apply(Group& node)
{
  traverse(node);
}

BuildCommandBufferVisitor::BuildCommandBufferVisitor(Surface* s, CommandBuffer* cb)
  : NodeVisitor{ AllChildren }, renderContext(s), commandBuffer{ cb }
{

}

void BuildCommandBufferVisitor::apply(GraphicsPipeline& node)
{
  commandBuffer->cmdBindPipeline(&node);
  traverse(node);
}

void BuildCommandBufferVisitor::apply(ComputePipeline& node)
{
  commandBuffer->cmdBindPipeline(&node);
  traverse(node);
}

void BuildCommandBufferVisitor::apply(AssetBufferNode& node)
{
  node.assetBuffer->cmdBindVertexIndexBuffer(renderContext.device, commandBuffer, node.renderMask, node.vertexBinding);
  traverse(node);
}