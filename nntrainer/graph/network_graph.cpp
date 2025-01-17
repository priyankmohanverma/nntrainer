// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (C) 2020 Jijoong Moon <jijoong.moon@samsung.com>
 *
 * @file    network_graph.h
 * @date    19 Oct 2020
 * @see     https://github.com/nnstreamer/nntrainer
 * @author  Jijoong Moon <jijoong.moon@samsung.com>
 * @bug     No known bugs except for NYI items
 * @brief   This is Network Graph Class for Neural Network
 *
 * @todo    Support multi-input graph.
 */

#include <sstream>

#include <activation_layer.h>
#include <addition_layer.h>
#include <bn_layer.h>
#include <concat_layer.h>
#include <cross_entropy_loss_layer.h>
#include <cross_entropy_sigmoid_loss_layer.h>
#include <cross_entropy_softmax_loss_layer.h>
#include <flatten_layer.h>
#include <input_layer.h>
#include <multiout_layer.h>
#include <network_graph.h>
#include <nntrainer_error.h>
#include <nntrainer_log.h>
#include <profiler.h>
#include <rnn.h>
#include <split_layer.h>
#include <time_dist.h>
#include <util_func.h>

#define LNODE(x) std::static_pointer_cast<LayerNode>(x)

namespace nntrainer {

int NetworkGraph::compile(const std::string &loss_type) {
  int status = ML_ERROR_NONE;

  status = isCompilable();
  NN_RETURN_STATUS();

  status = realizeGraph();
  NN_RETURN_STATUS();

  graph.realizeInputOutputNode();

  try {
    status = addLossLayer(loss_type);
    NN_RETURN_STATUS();
  } catch (const std::exception &e) {
    ml_loge("%s", e.what());
    status = ML_ERROR_INVALID_PARAMETER;
    NN_RETURN_STATUS();
  }

  graph.topologicalSort();

  countNonTrainableLayersAtBegin();
  setExecutionOrder();

  status = checkCompiledGraph();
  NN_RETURN_STATUS();

  compiled = true;

  return status;
}

void NetworkGraph::setExecutionOrder() {
  auto max_count = graph.size() * 3;
  /** @todo: remove backwarding count for non-trainble layers */
  for (auto iter = cbegin(); iter != cend(); iter++) {
    auto &node = *iter;
    auto order_idx = iter - cbegin();
    auto forward_order = order_idx;
    auto calc_gradient_order = max_count - ((order_idx + 1) * 2);
    /** calc derivative is called right after calc_gradient */
    auto calc_derivative_order = calc_gradient_order + 1;
    node->setExecutionOrder(
      {forward_order, calc_gradient_order, calc_derivative_order});
  }
}

void NetworkGraph::updateConnectionName(const std::string &from,
                                        const std::string &to) {
  for (auto iter = cbegin(); iter != cend(); iter++) {
    auto &lnode = *iter;
    if (istrequal(lnode->getName(), to))
      continue;
    lnode->updateInputLayers(from, to);
  }
}

void NetworkGraph::addDefaultInputLayers() {
  for (auto iter = cbegin() + 1; iter != cend(); iter++) {
    auto layer = *iter;
    auto prev_layer = *(iter - 1);
    if (layer->getNumInputConnections() == 0 &&
        !layer->hasInputShapeProperty()) {
      ml_logd("default input added %s->%s", prev_layer->getName().c_str(),
              layer->getName().c_str());
      layer->addInputLayers(prev_layer->getName());
    }
  }
}

void NetworkGraph::addLayerNode(std::unique_ptr<Layer> layer) {
  graph.addNode(std::make_unique<LayerNode>(std::move(layer)));
}

void NetworkGraph::countNonTrainableLayersAtBegin() {
  for (auto iter = cbegin(); iter != cend(); iter++) {
    // TODO: check if getTrainable() was set and if trainable weights exist,
    // then throw
    if ((*iter)->getTrainable() && (*iter)->supportBackwarding()) {
      skip_non_trainable_layers = iter - cbegin();
      return;
    }
  }

  skip_non_trainable_layers = graph.size();
}

int NetworkGraph::realizeActivationType(
  const std::shared_ptr<LayerNode> &in_node) {
  int status = ML_ERROR_NONE;

  ActivationType act = in_node->getActivationToBeRealized();

  if (act == ActivationType::ACT_NONE) {
    /// ActivationType::ACT_NONE does not need realization
    return ML_ERROR_NONE;
  }

  if (act == ActivationType::ACT_UNKNOWN) {
    ml_loge("cannot realize unknown activation type");
    return ML_ERROR_INVALID_PARAMETER;
  }

  if (in_node->getType() == ActivationLayer::type) {
    ml_loge("It is not allowed to realize activation layer, possibly layer is "
            "added right after activation");
    return ML_ERROR_INVALID_PARAMETER;
  }

  std::shared_ptr<LayerNode> lnode = createLayerNode(ActivationLayer::type);
  graph.ensureName(*lnode, in_node->getName());

  if (in_node->getDistribute()) {
    lnode->setProperty({"distribute=true"});
  }

  props::Activation act_prop;
  act_prop.set(act);
  lnode->setProperty({"activation=" + to_string(act_prop)});
  in_node->setProperty({"activation=none"});

  lnode->setInputLayers({in_node->getName()});
  /** output layers for layer obj will be set in setOutputLayers() */

  updateConnectionName(in_node->getName(), lnode->getName());
  graph.addNode(lnode, false);

  return status;
}

int NetworkGraph::realizeMultiOutputType(
  const std::shared_ptr<LayerNode> &in_node) {
  int status = ML_ERROR_NONE;
  /**
   * Multi-input works with time distribution layer by itself
   *
   */

  if (in_node->getNumOutputConnections() <= 1)
    return ML_ERROR_NONE;

  std::shared_ptr<LayerNode> lnode = createLayerNode(MultiOutLayer::type);
  graph.ensureName(*lnode, in_node->getName());

  lnode->setInputLayers({in_node->getName()});
  lnode->setOutputLayers(in_node->getOutputLayers());

  in_node->setOutputLayers({lnode->getName()});

  for (unsigned int i = 0; i < in_node->getNumOutputConnections(); ++i) {
    updateConnectionName(in_node->getName(), lnode->getName());
  }

  graph.addNode(lnode, false);

  return status;
}

int NetworkGraph::addLossLayer(const std::string &loss_type_) {
  for (unsigned int i = 0; i < graph.getNumOutputNodes(); ++i) {
    auto output_layer_node = LNODE(graph.getOutputNode(i));
    std::string loss_type = loss_type_;

    if (output_layer_node->requireLabel())
      continue;

    if (loss_type.empty())
      continue;

    auto second_to_last_layer_node = output_layer_node;
    bool is_cross_entropy_loss =
      istrequal(loss_type, CrossEntropyLossLayer::type);
    if (is_cross_entropy_loss) {
      auto type = output_layer_node->getType();

      if (type != ActivationLayer::type) {
        throw exception::not_supported(
          "Error: Cross Entropy need last layer to have softmax or sigmoid"
          "activation.");
      }

      switch (output_layer_node->getActivationType()) {
      case ActivationType::ACT_SIGMOID:
        loss_type = CrossEntropySigmoidLossLayer::type;
        break;
      case ActivationType::ACT_SOFTMAX:
        loss_type = CrossEntropySoftmaxLossLayer::type;
        break;
      default:
        throw exception::not_supported(
          "Error: Cross Entropy not supported without softmax or sigmoid.");
      }

      second_to_last_layer_node =
        LNODE(graph.getNode(output_layer_node->getInputLayers()[0]));
    }

    std::shared_ptr<LayerNode> lnode = createLayerNode(loss_type);
    graph.ensureName(*lnode);

    if (second_to_last_layer_node->getDistribute()) {
      lnode->setProperty({"distribute=true"});
    }

    second_to_last_layer_node->setOutputLayers({lnode->getName()});
    lnode->setInputLayers({second_to_last_layer_node->getName()});

    if (is_cross_entropy_loss) {
      graph.replaceNode(output_layer_node, lnode);
    } else {
      graph.addNode(lnode, false);
    }
    graph.replaceOutputNode(i, lnode);
  }

  return ML_ERROR_NONE;
}

void NetworkGraph::setOutputLayers() {
  for (auto iter_idx = cbegin(); iter_idx != cend(); iter_idx++) {
    auto &layer_idx = *iter_idx;
    for (auto iter_i = cbegin(); iter_i != cend(); iter_i++) {
      auto &layer_i = *iter_i;
      if (istrequal(layer_i->getName(), layer_idx->getName()))
        continue;
      for (unsigned int j = 0; j < layer_i->getNumInputConnections(); ++j) {
        if (istrequal(layer_i->getInputLayers()[j], layer_idx->getName())) {
          bool already_exist = false;
          for (unsigned int k = 0; k < layer_idx->getNumOutputConnections();
               ++k) {
            if (istrequal(layer_idx->getOutputLayers()[k],
                          layer_i->getName())) {
              already_exist = true;
              break;
            }
          }

          if (!already_exist)
            layer_idx->addOutputLayers(layer_i->getName());
        }
      }
    }
  }
}

int NetworkGraph::isCompilable() {
  if (compiled) {
    ml_loge("Graph is already compiled");
    return ML_ERROR_NOT_SUPPORTED;
  }

  if (graph.empty()) {
    ml_loge("Graph is empty");
    return ML_ERROR_INVALID_PARAMETER;
  }

  return ML_ERROR_NONE;
}

int NetworkGraph::checkCompiledGraph() {
  /** Dimension of input layers must be known */
  for (auto iter = cbegin(); iter != cend(); iter++) {
    auto lnode = (*iter);
    if (lnode->getNumInputConnections() == 0) {
      if (!lnode->hasInputShapeProperty()) {
        ml_loge("Layer with no inbound connection need input_shape property");
        return ML_ERROR_INVALID_PARAMETER;
      }
    }
  }

  return ML_ERROR_NONE;
}

int NetworkGraph::checkInitializedGraph() {
  /** accumulate all the nodes which must support backwarding */
  std::unordered_set<std::string> must_support_backwarding;

  /**
   * if a node is trainable, then all the nodes ahead of it must support
   * backwarding operation
   */
  for (auto iter = cbegin(); iter != cend(); iter++) {
    auto lnode = (*iter);
    if (lnode->getTrainable() ||
        must_support_backwarding.find(lnode->getName()) !=
          must_support_backwarding.end()) {
      for (auto const &out_layer : lnode->getOutputLayers()) {
        must_support_backwarding.insert(out_layer);
      }
    }
  }

  /** verify all the required nodes support backwarding */
  for (auto const &node_name : must_support_backwarding) {
    if (!LNODE(graph.getNode(node_name))->supportBackwarding()) {
      ml_loge(
        "Backwaring required from layer which doesn't support backwarding");
      return ML_ERROR_INVALID_PARAMETER;
    }
  }

  return ML_ERROR_NONE;
}

int NetworkGraph::realizeGraph() {
  int status = ML_ERROR_NONE;

  addDefaultInputLayers();

  /**
   * invariant: the new realized nodes are added to the end,
   * otherwise this iteration becomes invalid. So, every iteration must be
   * fresh iterator as vector resize invalidates all the iterators.
   */
  for (unsigned int i = 0; i < graph.size(); ++i) {
    auto const &lnode = LNODE(*(cbegin() + i));
    ml_logd("layer name: %s", lnode->getName().c_str());

    /** If a layer does not has input nodes, then it must have input dimension
     */
    if (lnode->getNumInputConnections() == 0) {
      if (!lnode->hasInputShapeProperty()) {
        ml_loge("Input Dimension must be set");
        status = ML_ERROR_INVALID_PARAMETER;
        NN_RETURN_STATUS();
      }
    }

    if (lnode->getType() != ActivationLayer::type) {
      status = realizeActivationType(lnode);
      NN_RETURN_STATUS();
    }
  }

  try {
    setOutputLayers();
  } catch (std::exception &e) {
    ml_loge("setting output layer failed, reason: %s", e.what());
    return ML_ERROR_INVALID_PARAMETER;
  }

  /**
   * invariant: the new realized nodes are added to the end,
   * otherwise this iteration becomes invalid. So, every iteration must be
   * fresh iterator as vector resize invalidates all the iterators.
   */
  for (unsigned int i = 0; i < graph.size(); ++i) {
    auto const &lnode = LNODE(*(cbegin() + i));
    if (lnode->getType() != MultiOutLayer::type &&
        lnode->getType() != SplitLayer::type) {
      status = realizeMultiOutputType(lnode);
      NN_RETURN_STATUS();
    }
  }
  /// @todo add check that input_layers <-> output_layers does match.
  /// @todo check whether graph has a cycle or graph is seperated to subgraph

  return status;
}

void NetworkGraph::setBatchSize(unsigned int batch_size) {
  if (batch_size == this->batch_size)
    return;

  this->batch_size = batch_size;
  if (!input_list.empty() && getInputDimension()[0].batch() == batch_size)
    return;

  auto allocated = tensor_manager->isAllocated();

  if (allocated)
    deallocateTensors();

  for (auto iter = cbegin(); iter != cend(); iter++) {
    (*iter)->setBatch(batch_size);
    if ((*iter)->isFinalized()) {
      const RunLayerContext &context = (*iter)->getRunContext();
      // resize tensors spec
      for (unsigned int idx = 0; idx < context.getNumTensors(); idx++) {
        auto const &ts = context.getTensor(idx);
        tensor_manager->setBatchSize(ts.getName(), ts.getDim().batch());
        if (context.tensorHasGradient(idx)) {
          auto const &ts_grad = context.getTensorGrad(idx);
          tensor_manager->setBatchSize(ts_grad.getName(),
                                       ts_grad.getDim().batch());
        }
      }
    }
  }
  /// resize input and output spec
  tensor_manager->setBatchSize(batch_size);

  if (allocated)
    allocateTensors(exec_mode);

  /** update input and label dimensions */
  for (unsigned int idx = 0; idx < input_list.size(); idx++)
    input_dims[idx] = tensor_manager->getTensor(input_list[idx])->getDim();
  for (unsigned int idx = 0; idx < label_list.size(); idx++)
    label_dims[idx] = tensor_manager->getTensor(label_list[idx])->getDim();
}

void NetworkGraph::applyGradientsOnLastAccess(
  LayerNode *node, std::function<void(Weight &)> apply_func) {
  auto &rc = node->getRunContext();
  auto num_weight = rc.getNumWeights();
  for (unsigned i = 0; i < num_weight; ++i) {
    if (!rc.weightHasGradient(i)) {
      continue;
    }

    if (rc.isWeightDependent(i)) {
      /// @note instead of checking the last access of the weight, checking
      /// if weights are dependent to others to minimize overhead.
      /// this logic assums that the source of the dependent weight must be
      /// prior to the dependent.
      continue;
    }

    apply_func(rc.getWeightObject(i));
  }
}

sharedConstTensors NetworkGraph::forwarding(bool training) const {
  for (auto iter = cbegin(); iter != cend(); iter++) {
    auto const &ln = *iter;
    ln->forwarding(training);
  }

  sharedConstTensors out;
  for (unsigned int i = 0; i < graph.getNumOutputNodes(); ++i) {
    auto const &output_layer_node = LNODE(graph.getOutputNode(i));
    for (unsigned int j = 0; j < output_layer_node->getNumOutputs(); ++j) {
      out.push_back(MAKE_SHARED_TENSOR(output_layer_node->getOutput(j)));
    }
  }

  return out;
}

std::vector<TensorDim> NetworkGraph::getInputDimension() const {
  NNTR_THROW_IF(input_dims.empty(), std::invalid_argument)
    << "[NetworkGraph] the graph has no node identified as input!";
  return input_dims;
}

unsigned int NetworkGraph::getBatchSize() const { return batch_size; }

std::vector<TensorDim> NetworkGraph::getOutputDimension() const {
  NNTR_THROW_IF(label_dims.empty(), std::invalid_argument)
    << "[NetworkGraph] the graph has no node identified as output!";
  /// for now, outputting label_dims works, later label dim will be different
  /// from output dimension
  return label_dims;
}

std::vector<std::shared_ptr<LayerNode>>
NetworkGraph::getUnsortedLayers(const std::string &input_layer,
                                const std::string &output_layer) const {
  /// @fixme: this won't work if input, output layers are not in order
  /// Further, this function must be removed. There should be rather
  /// getAllNames and getLayerByName instead of getUnsortedLayers.

  /** count layers after output layer */
  unsigned int num_layers_remove_end = 0;
  if (!output_layer.empty()) {
    for (auto iter = graph.crbegin(); iter != graph.crend(); iter++) {
      if ((*iter)->getName() != output_layer)
        num_layers_remove_end++;
      else
        break;
    }
  }

  if (num_layers_remove_end == graph.size())
    return {};

  /** count layers before input layer */
  unsigned int num_layers_remove_start = 0;
  if (!input_layer.empty()) {
    for (auto iter = graph.cbegin();
         iter != graph.cend() - num_layers_remove_end; iter++) {
      if ((*iter)->getName() != input_layer)
        num_layers_remove_start++;
      else
        break;
    }
  }

  /** copy the graph and return */
  std::vector<std::shared_ptr<LayerNode>> ret;
  std::transform(graph.cbegin() + num_layers_remove_start,
                 graph.cend() - num_layers_remove_end, std::back_inserter(ret),
                 [](auto const &elem) { return LNODE(elem); });

  return ret;
}

std::vector<std::shared_ptr<LayerNode>> NetworkGraph::getLayerNodes() const {
  return std::vector<std::shared_ptr<LayerNode>>(cbegin(), cend());
}

void NetworkGraph::extendGraph(std::vector<std::shared_ptr<LayerNode>> ex_graph,
                               std::string &prefix) {
  if (compiled)
    throw std::runtime_error("Cannot modify graph after compile");

  /**
   * The input_layers for ex_graph[0] here is provided to the backbone by the
   * ini file and is overwritten here by the model loader for connection
   * making.
   *
   * This loop intends to connect a new backbone to be added with an old
   * backbone.
   */
  auto &layer0_in = ex_graph[0]->getInputLayers();
  for (unsigned int i = 0; i < layer0_in.size(); ++i) {
    if (sub_in_out.find(layer0_in[i]) != sub_in_out.end()) {
      ex_graph[0]->updateInputLayers(i, sub_in_out[layer0_in[i]]);
    } else if (!graph.verifyNode(layer0_in[i])) {
      throw std::runtime_error("Input layer name for backbone not found.");
    }
  }

  /** Insert the layer to the graph */
  for (auto &layernode : ex_graph) {
    /**
     * Add prefix to the existing layer name,
     * and ensure it is unique in this new ex_graph
     */
    std::string orig_name = prefix + layernode->getName();
    graph.ensureName(*layernode, prefix, "", true);
    sub_in_out.insert(std::make_pair(orig_name, layernode->getName()));

    auto &input_layers = layernode->getInputLayers();
    for (unsigned int i = 0; i < input_layers.size(); ++i) {
      if (sub_in_out.find(prefix + input_layers[i]) != sub_in_out.end()) {
        layernode->updateInputLayers(
          i, sub_in_out[prefix + layernode->getInputLayers()[i]]);
      } else if (!graph.verifyNode(layernode->getInputLayers()[i])) {
        throw std::runtime_error("Input layer name for backbone not found.");
      }
    }

    graph.addNode(layernode, false);
  }

  /** This allows connecting a layer to the backbone */
  sub_in_out.insert(
    std::make_pair(prefix, graph.getNode(graph.size() - 1)->getName()));

  /** @todo Update shared_from node name as well */
  /** @todo Add test for this */
}

void NetworkGraph::addLayer(std::shared_ptr<LayerNode> layer) {
  if (compiled)
    throw std::runtime_error("Cannot modify graph after compile");

  /** Insert the layer to the graph */
  graph.addNode(layer);
}

void NetworkGraph::inPlaceOptimize() {
  // TODO: update this after initial verification, this is deprecated for now.
  return;

#if 0
  for (auto iter = cbegin(); iter != cend(); iter++) {
    auto layer_node = *iter;
    auto &l = layer_node->getObject();
    std::string l_type = l->getType();

    /**
     * @todo concat/multi-output layer can be made in-place but with special
     * handling where these layers can become no-op.
     * consider this optimization for later
     */

    if (l->supportInPlace()) {
      /** @note assumes layer to be optimized is only for single in/out tensor
       */
      if (layer_node->getNumInputConnections() != 1)
        throw std::runtime_error("Internal error in the formed graph");

      auto prev_node = getLayerNode(layer_node->getInputLayers()[0]);
      auto &prev_layer =
        getLayerNode(layer_node->getInputLayers()[0])->getObject();

      unsigned int loc;
      auto layer_name = layer_node->getName();
      auto &output_layers = prev_node->getOutputLayers();
      for (loc = 0; loc < output_layers.size(); ++loc)
        if (output_layers[loc] == layer_name)
          break;

      if (loc == output_layers.size())
        throw std::runtime_error("Internal error in the formed graph.");

      /** Previous layer cannot be input layer for in-place layer */
      if (prev_node->getType() == InputLayer::type)
        continue;

      /** Two layers cant work in-place consecutively */
      if (prev_node->supportInPlace())
        continue;

      /** Share tensor with next layer */
      /**
       * Assume two layers, L1 and L2, with I and O corresponding to the layer
       * outputs. Assume L2 to be the layer, needing in-place optimization.
       */
      if (l_type == BatchNormalizationLayer::type) {
        /**
         * With batch normalization, neither input nor output of the layer are
         * requried for calculatin gradient and derivative. Just input
         * derivative is required. In this scenraio, L2 is assumed to be batch
         * normalization layer, and L1 is assumed to be a non-in-place layer.
         * Hence, L1 layer's output and L2 layer's input var_grad are modified.
         */
        auto &inplace_shared_vg_ptr = l->net_hidden[0];
        l->net_input[0] = inplace_shared_vg_ptr;             /// I2 = O2
        prev_layer->net_hidden[loc] = inplace_shared_vg_ptr; /// O1 = O2
      } else if (l_type == ActivationLayer::type) {
        /**
         * For activation layer, output of the layer and input derivative, both
         * , are requried for calculating the gradient and derivative. In this
         * scenraio, L2 is assumed to be activation layer, and L1 is assumed to
         * be a non-in-place layer.
         * Hence, L1 layer's output and L2 layer's input var_grad are
         * differently. L1 layer operates out of place and share the memory for
         * its output (O1.V) and input derivative (O1.G). L2 layer operates
         * in-place and use a common shared derivative memory for their
         * derivatives (handled in manager.cpp).
         */
        /**
         * @note As this updates the tensors used in the prev_layer->net_hidden
         * (O1), the tensors inside l->net_input (I2) are automatically updated
         * as they share the same var_grad object.
         */
        auto &inplace_shared_vg = *l->net_hidden[0].get();
        prev_layer->net_hidden[loc]->updateVariableByVariable(
          inplace_shared_vg); /// O1.G = O2.V
        prev_layer->net_hidden[loc]->updateGradientByVariable(
          inplace_shared_vg); /// O1.V = O2.V
      } else {
        std::stringstream ss;
        ss << l_type;
        ss << " layer is not supported for in-place optimization";
        throw std::runtime_error(ss.str());
      }

      /** Untrack the memory for this layer */
      manager.untrackLayerInOuts(prev_node->getName());
    }
  }
#endif
}

std::vector<Var_Grad *>
NetworkGraph::finalizeContext(const std::shared_ptr<LayerNode> &lnode,
                              const std::vector<Var_Grad *> &prev_inputs) {
  const GraphNode &gnode = *lnode.get();
  std::vector<TensorDim> input_dims;
  input_dims.reserve(prev_inputs.size());
  std::transform(prev_inputs.begin(), prev_inputs.end(),
                 std::back_inserter(input_dims),
                 [](const Var_Grad *vg) { return vg->getDim(); });

  /** finalize the layer and get the final context */
  auto init_context = lnode->finalize(input_dims);

  /**
   * Request manager for either a pre-allocated output as input or a newly
   * allocated input. This is necesary for manager to know when this input node
   * is going to be used.
   */
  std::vector<std::string> input_names;
  input_names.reserve(prev_inputs.size());
  std::transform(prev_inputs.begin(), prev_inputs.end(),
                 std::back_inserter(input_names),
                 [](auto const &vg) { return vg->getName(); });
  const std::vector<Var_Grad *> &inputs = tensor_manager->requestInputs(
    gnode, init_context.getInputDimensions(), input_names);

  /**
   * In-Place optimizations
   */
  std::vector<std::string> inputs_name;
  if (lnode->getType() == FlattenLayer::type ||
      lnode->getType() == InputLayer::type)
    std::transform(inputs.begin(), inputs.end(),
                   std::back_inserter(inputs_name),
                   [](const Var_Grad *val) { return val->getName(); });

  /**
   * Request manager for either a pre-allocated input as output or a newly
   * allocated input. This is necesary for manager to know when this output node
   * is going to be used with in-place optimizations.
   */
  const std::vector<Var_Grad *> &outputs = tensor_manager->requestOutputs(
    gnode, init_context.getOutputDimensions(), inputs_name);

  /** create shared weight names if requested */
  std::vector<std::string> shared_weight_names;
  std::vector<std::string> shared_tensor_names;
  if (auto shared_node_str = lnode->getSharedFrom(); !shared_node_str.empty()) {
    auto shared_node = getLayerNode(shared_node_str).get();
    NNTR_THROW_IF(shared_node == nullptr, std::invalid_argument)
      << "shared_node requested but it is not registered in the graph, name: "
      << shared_node_str << " requested from " << lnode->getName();
    NNTR_THROW_IF(shared_node->getType() != lnode->getType(),
                  std::invalid_argument)
      << " shared_node and lnode type mismatch, source node type: "
      << shared_node->getType() << " depedent node type: " << lnode->getType()
      << " depedent node name: " << lnode->getName();
    NNTR_THROW_IF(!shared_node->isFinalized(), std::invalid_argument)
      << "shared node must be prior to the dependent node and it should be "
         "finalized beforehand, shared node name: "
      << shared_node_str << " dependent node name: " << lnode->getName();
    auto num_weight = shared_node->getNumWeights();
    shared_weight_names.reserve(num_weight);
    for (auto i = 0u; i < num_weight; ++i) {
      shared_weight_names.emplace_back(shared_node->getWeightName(i));
    }

    auto &rc = shared_node->getRunContext();

    /// @fixme tensor should be only shared if context explicitly requested to
    /// do so. This has to be added to the part of tensor spec, other wise it
    /// will break many things
    auto num_tensors = rc.getNumTensors();
    for (auto i = 0u; i < num_tensors; ++i) {
      shared_tensor_names.emplace_back(rc.getTensorName(i));
    }
  }

  lnode->configureRunContext(
    // TODO: update weights spec for trainable based on layer trainable prop
    tensor_manager->requestWeights(gnode, init_context.getWeightsSpec(),
                                   lnode->getTrainable(), shared_weight_names),
    inputs, outputs,
    tensor_manager->requestTensors(gnode, init_context.getTensorsSpec(),
                                   shared_tensor_names));

  return outputs;
}

int NetworkGraph::initialize(
  const std::vector<std::string> &model_input_names,
  const std::vector<std::string> &model_label_names) {
  int status = ML_ERROR_NONE;
  /**
   * this contains the map from node name to its input tensor names
   * @note: these input tensors have already been allocated
   */
  std::unordered_map<std::string, std::vector<Var_Grad *>> input_map;

  /** check if the given config of node is of input node */
  auto is_input_node = [](const LayerNode *node) -> bool {
    return node->getInputConnections().empty();
  };

  for (unsigned int idx = 0; idx < graph.size(); ++idx) {
    std::vector<Var_Grad *> inputs = {};
    auto const &lnode = getSortedLayerNode(idx);
    ml_logd("layer name : %s", lnode->getName().c_str());

    /**
     * Set input dimension for all the layers.
     * For input layer, as input dimension is known, set input tensor.
     */
    if (!is_input_node(lnode.get())) {
      if (input_map.find(lnode->getName()) == input_map.end())
        throw std::runtime_error("Cannot find input buffers for the node");
      inputs = input_map.at(lnode->getName());
    }

    /**
     * Initialize all the layers, allocate output tensors for each layer
     * init2and add optimizer related weights for the layer
     */
    const std::vector<Var_Grad *> &outputs = finalizeContext(lnode, inputs);

    /** no need to update input_map for the last layer */
    if (idx == graph.size() - 1)
      break;

    auto &output_layers = lnode->getOutputLayers();
    for (unsigned int i = 0; i < output_layers.size(); ++i) {
      auto out_layer_node = getLayerNode(output_layers[i]);
      if (input_map.find(output_layers[i]) == input_map.end())
        input_map.insert({output_layers[i], {}});

      unsigned int j = 0;
      for (; j < out_layer_node->getNumInputConnections(); ++j) {
        if (istrequal(out_layer_node->getInputLayers()[j], lnode->getName())) {
          break;
        }
      }

      auto &in_map = input_map.at(output_layers[i]);
      in_map.resize(out_layer_node->getNumInputConnections());
      in_map[j] = outputs[i];
    }
  }

  /**** identify model input / output to be set externally later ****/
  auto identify_as_model_input = [this](LayerNode *node) {
    auto num_input = node->getNumInputs();
    NNTR_THROW_IF(num_input != 1, std::invalid_argument)
      << "Input layer is supposed to have exactly one input, but more then "
         "one input detected, num inputs: "
      << num_input;

    input_list.push_back(node->getInput(0).getName());
    input_dims.push_back(node->getInputDimensions()[0]);
  };

  auto is_label_node = [](LayerNode *node) { return node->requireLabel(); };

  auto identify_as_model_label = [this](LayerNode *node) {
    /// @todo change this as lnode->getNumLabels of sorts
    auto num_label = node->getNumOutputs();
    NNTR_THROW_IF(!node->getOutputConnections().empty(), std::invalid_argument)
      << "label layer is supposed to be a leaf for now";
    NNTR_THROW_IF(num_label != 1, std::invalid_argument)
      << "label layer is supposed to have exactly one label, but more then "
         "one label detected, num labels: "
      << num_label;

    /// @todo implement and use getLabel(0) instead.
    output_list.push_back(node->getOutput(0).getName());
    label_list.push_back(node->getOutputGrad(0).getName());
    label_dims.push_back(node->getOutputDimensions()[0]);
  };

  auto identify_external_tensors = [this](const std::vector<std::string> &names,
                                          auto &&pred, auto &&identify) {
    if (names.empty()) {
      for (unsigned int i = 0; i < graph.size(); ++i) {
        auto lnode = getSortedLayerNode(i).get();
        if (!pred(lnode)) {
          continue;
        }
        /// when name is empty, we identify everything as the node, all of them
        /// must be having identical dimensions
        identify(lnode);
      }
    } else {
      for (auto &name : names) {
        auto lnode = getLayerNode(name).get();
        NNTR_THROW_IF(!pred(lnode), std::invalid_argument)
          << "given node is not of that kind, name: " << name;
        identify(lnode);
      }
      unsigned int num_node_of_kind = 0;
      for (unsigned int i = 0; i < graph.size(); ++i) {
        auto lnode = getSortedLayerNode(i).get();
        if (!pred(lnode)) {
          continue;
        }
        num_node_of_kind++;
      }
      NNTR_THROW_IF(num_node_of_kind != names.size(), std::invalid_argument)
        << "names given but there are not identified node of the kind, num "
           "node of kind: "
        << num_node_of_kind << " identifier size: " << names.size();
    }
  };

  identify_external_tensors(model_input_names, is_input_node,
                            identify_as_model_input);
  identify_external_tensors(model_label_names, is_label_node,
                            identify_as_model_label);

  if (status != ML_ERROR_NONE)
    return status;

  return checkInitializedGraph();
}

void NetworkGraph::setExternalTensors(const std::vector<Tensor> &data,
                                      const std::vector<std::string> names) {

  /// feed or clear label
  for (unsigned int idx = 0; idx < names.size(); idx++) {
    if (data.empty())
      tensor_manager->setExternalTensor(names[idx], Tensor());
    else if (data.size() == 1)
      tensor_manager->setExternalTensor(names[idx], data[0]);
    else
      tensor_manager->setExternalTensor(names[idx], data[idx]);
  }
}

void NetworkGraph::setInputsLabels(const std::vector<Tensor> &inputs,
                                   const std::vector<Tensor> &labels) {

  NNTR_THROW_IF(labels.size() > 1 && labels.size() != label_list.size(),
                std::invalid_argument)
    << "label size does not match with the network requirements"
    << " label size: " << labels.size()
    << " requirements size: " << label_list.size();

  NNTR_THROW_IF(inputs.size() > 1 && inputs.size() != input_list.size(),
                std::invalid_argument)
    << "input size does not match with the network requirements"
    << " input size: " << inputs.size()
    << " requirements size: " << input_list.size();

  setExternalTensors(inputs, input_list);
  setExternalTensors(labels, label_list);
  tensor_manager->updateExternalTensors();
}

void NetworkGraph::setInputsLabels(sharedConstTensors &inputs,
                                   sharedConstTensors &labels) {

  std::vector<Tensor> ins;
  std::transform(inputs.begin(), inputs.end(), std::back_inserter(ins),
                 [](auto const &val) { return *val.get(); });

  std::vector<Tensor> labs;
  std::transform(labels.begin(), labels.end(), std::back_inserter(labs),
                 [](auto const &val) { return *val.get(); });

  setInputsLabels(ins, labs);
}

std::vector<Tensor> NetworkGraph::getOutputTensors() const {
  std::vector<Tensor> output_tensors;
  output_tensors.reserve(output_list.size());

  for (auto const &name : output_list)
    output_tensors.push_back(*tensor_manager->getTensor(name));

  return output_tensors;
}

} /* namespace nntrainer */
