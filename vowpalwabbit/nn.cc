/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <sstream>

#include "constant.h"
#include "oaa.h"
#include "simple_label.h"
#include "cache.h"
#include "v_hashmap.h"

using namespace std;

namespace NN {
  //nonreentrant
  size_t k=0;
  size_t increment=0;
  size_t total_increment=0;
  loss_function* squared_loss;
  example output_layer;
  const float hidden_min_activation = -3;
  const float hidden_max_activation = 3;
  const int nn_constant = 533357803;
  bool dropout = false;
  unsigned short xsubi[3];
  unsigned short save_xsubi[3];
  size_t nn_current_pass = 0;

  #ifdef _WIN32
    inline double erand48(unsigned short us[]) { return rand() / (double)RAND_MAX; }
    inline double drand48() { return rand() / (double)RAND_MAX; }
    inline unsigned short * seed48(unsigned short seed[]) { srand((int)seed); unsigned short empty[3]; return empty; }
  #endif

  static void
  free_squared_loss (void)
  {
    delete squared_loss;
  }

  static void
  free_output_layer (void)
  {
    free (output_layer.indices.begin);
    free (output_layer.atomics[nn_output_namespace].begin);
  }

#define cast_uint32_t static_cast<uint32_t>

  static inline float
  fastpow2 (float p)
  {
    float offset = (p < 0) ? 1.0f : 0.0f;
    float clipp = (p < -126) ? -126.0f : p;
    int w = clipp;
    float z = clipp - w + offset;
    union { uint32_t i; float f; } v = { cast_uint32_t ( (1 << 23) * (clipp + 121.2740575f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z) ) };

    return v.f;
  }

  static inline float
  fastexp (float p)
  {
    return fastpow2 (1.442695040f * p);
  }

  static inline float
  fasttanh (float p)
  {
    return -1.0f + 2.0f / (1.0f + fastexp (-2.0f * p));
  }

  void scale_example_indicies(bool audit, example* ec, size_t amount)
  {
    for (size_t* i = ec->indices.begin; i != ec->indices.end; i++) 
      {
        feature* end = ec->atomics[*i].end;
        for (feature* f = ec->atomics[*i].begin; f!= end; f++)
          f->weight_index *= amount;
      }
    if (audit)
      {
        for (size_t* i = ec->indices.begin; i != ec->indices.end; i++) 
          if (ec->audit_features[*i].begin != ec->audit_features[*i].end)
            for (audit_data *f = ec->audit_features[*i].begin; f != ec->audit_features[*i].end; f++)
              f->weight_index *= amount;
      }
  }

  void (*base_learner)(void*,example*) = NULL;

  void learn_with_output(vw*all, example* ec, bool shouldOutput)
  {
    if (command_example(*all, ec)) {
      return;
    }

    if (all->bfgs && ec->pass != nn_current_pass) {
      memcpy (xsubi, save_xsubi, sizeof (xsubi));
      nn_current_pass = ec->pass;
    }

    label_data* ld = (label_data*)ec->ld;
    float save_label = ld->label;
    void (*save_set_minmax) (shared_data*, float) = all->set_minmax;
    float save_min_label;
    float save_max_label;
    float dropscale = dropout ? 2.0 : 1.0;
    loss_function* save_loss = all->loss;

    float* hidden_units = (float*) alloca (k * sizeof (float));
    bool* dropped_out = (bool*) alloca (k * sizeof (bool));
  
    string outputString;
    stringstream outputStringStream(outputString);

    all->set_minmax = noop_mm;
    all->loss = squared_loss;
    save_min_label = all->sd->min_label;
    all->sd->min_label = hidden_min_activation;
    save_max_label = all->sd->max_label;
    all->sd->max_label = hidden_max_activation;
    ld->label = FLT_MAX;
    for (unsigned int i = 0; i < k; ++i)
      {
        if (i != 0)
          update_example_indicies(all->audit, ec, increment);

        base_learner(all,ec);
        hidden_units[i] = finalize_prediction (*all, ec->partial_prediction);

        dropped_out[i] = (dropout && erand48 (xsubi) < 0.5);

        if (shouldOutput) {
          if (i > 0) outputStringStream << ' ';
          outputStringStream << i << ':' << ec->partial_prediction << ',' << fasttanh (hidden_units[i]);
        }

        ec->partial_prediction = 0;
      }
    ld->label = save_label;
    all->loss = save_loss;
    all->set_minmax = save_set_minmax;
    all->sd->min_label = save_min_label;
    all->sd->max_label = save_max_label;

    bool converse = false;

CONVERSE: // That's right, I'm using goto.  So sue me.

    output_layer.ld = ec->ld;
    output_layer.total_sum_feat_sq = 1;
    output_layer.sum_feat_sq[nn_output_namespace] = 1;

    for (unsigned int i = 0; i < k; ++i)
      {
        float sigmah = 
          (dropped_out[i]) ? 0.0 : dropscale * fasttanh (hidden_units[i]);
        output_layer.atomics[nn_output_namespace][i+1].x = sigmah;

        output_layer.total_sum_feat_sq += sigmah * sigmah;
        output_layer.sum_feat_sq[nn_output_namespace] += sigmah * sigmah;
      }

    output_layer.pass = ec->pass;
    output_layer.partial_prediction = 0;
    output_layer.eta_round = ec->eta_round;
    output_layer.eta_global = ec->eta_global;
    output_layer.global_weight = ec->global_weight;
    output_layer.example_t = ec->example_t;
    base_learner(all,&output_layer);
    output_layer.final_prediction = finalize_prediction (*all, output_layer.partial_prediction);
    output_layer.ld = 0;

    if (shouldOutput) {
      outputStringStream << ' ' << output_layer.partial_prediction;
      all->print_text(all->raw_prediction, outputStringStream.str(), ec->tag);
    }

    if (all->training && ld->label != FLT_MAX) {
      float gradient = all->loss->first_derivative(all->sd, 
                                                   output_layer.final_prediction,
                                                   ld->label);

      if (fabs (gradient) > 0) {
        all->loss = squared_loss;
        all->set_minmax = noop_mm;
        save_min_label = all->sd->min_label;
        all->sd->min_label = hidden_min_activation;
        save_max_label = all->sd->max_label;
        all->sd->max_label = hidden_max_activation;

        for (unsigned int i = k; i > 0; --i) {
          if (! dropped_out[i-1]) {
            float sigmah = 
              output_layer.atomics[nn_output_namespace][i].x / dropscale;
            float sigmahprime = dropscale * (1.0 - sigmah * sigmah);
            float nu = all->reg.weight_vectors[output_layer.atomics[nn_output_namespace][i].weight_index & all->weight_mask];
            float gradhw = 0.5 * nu * gradient * sigmahprime;

            ld->label = finalize_prediction (*all, hidden_units[i-1] - gradhw);
            if (ld->label != hidden_units[i-1]) {
              ec->partial_prediction = 0.;
              base_learner(all,ec);
            }
          }
          if (i != 1) {
            update_example_indicies(all->audit, ec, -increment);
          }
        }

        all->loss = save_loss;
        all->set_minmax = save_set_minmax;
        all->sd->min_label = save_min_label;
        all->sd->max_label = save_max_label;
      }
      else 
        update_example_indicies(all->audit, ec, -total_increment);
    }
    else 
      update_example_indicies(all->audit, ec, -total_increment);

    ec->partial_prediction = output_layer.partial_prediction;
    ec->final_prediction = output_layer.final_prediction;
    ec->loss = output_layer.loss;
    ld->label = save_label;

    if (dropout && ! converse)
      {
        update_example_indicies (all->audit, ec, total_increment);

        for (unsigned int i = 0; i < k; ++i)
          {
            dropped_out[i] = ! dropped_out[i];
          }

        converse = true;
        goto CONVERSE;
      }
  }

  void learn(void*a, example* ec) {
    vw* all = (vw*)a;
    learn_with_output(all, ec, false);
  }

  void drive_nn(void *in)
  {
    vw* all = (vw*)in;
    example* ec = NULL;
    while ( true )
      {
        if ((ec = get_example(all->p)) != NULL)//semiblocking operation.
          {
            learn_with_output(all, ec, all->raw_prediction > 0);
            int save_raw_prediction = all->raw_prediction;
            all->raw_prediction = -1;
            return_simple_example(*all, ec);
            all->raw_prediction = save_raw_prediction;
          }
        else if (parser_done(all->p))
	  return;
        else 
          ;
      }
  }

  void parse_flags(vw& all, std::vector<std::string>&opts, po::variables_map& vm, po::variables_map& vm_file)
  {
    po::options_description desc("NN options");
    desc.add_options()
      ("dropout", "Train or test sigmoidal feedforward network using dropout.")
      ("meanfield", "Train or test sigmoidal feedforward network using mean field.");

    po::parsed_options parsed = po::command_line_parser(opts).
      style(po::command_line_style::default_style ^ po::command_line_style::allow_guessing).
      options(desc).allow_unregistered().run();
    opts = po::collect_unrecognized(parsed.options, po::include_positional);
    po::store(parsed, vm);
    po::notify(vm);

    po::parsed_options parsed_file = po::command_line_parser(all.options_from_file_argc,all.options_from_file_argv).
      style(po::command_line_style::default_style ^ po::command_line_style::allow_guessing).
      options(desc).allow_unregistered().run();
    po::store(parsed_file, vm_file);
    po::notify(vm_file);

    //first parse for number of hidden units
    k = 0;
    if( vm_file.count("nn") ) {
      k = vm_file["nn"].as<size_t>();
      if( vm.count("nn") && vm["nn"].as<size_t>() != k )
        std::cerr << "warning: you specified a different number of hidden units through --nn than the one loaded from predictor. Pursuing with loaded value of: " << k << endl;
    }
    else {
      k = vm["nn"].as<size_t>();

      //append nn with nb_actions to options_from_file so it is saved to regressor later
      std::stringstream ss;
      ss << " --nn " << k;
      all.options_from_file.append(ss.str());
    }

    if( vm_file.count("dropout") ) {
      dropout = all.training || vm.count("dropout");

      if (! dropout && ! vm.count("meanfield") && ! all.quiet) 
        std::cerr << "using mean field for testing, specify --dropout explicitly to override" << std::endl;
    }
    else if ( vm.count("dropout") ) {
      dropout = true;

      std::stringstream ss;
      ss << " --dropout ";
      all.options_from_file.append(ss.str());
    }

    if ( vm.count("meanfield") ) {
      dropout = false;
      if (! all.quiet) 
        std::cerr << "using mean field for neural network " 
                  << (all.training ? "training" : "testing") 
                  << std::endl;
    }

    if (dropout) {
      if (! all.quiet)
        std::cerr << "using dropout for neural network "
                  << (all.training ? "training" : "testing") 
                  << std::endl;
    }

    all.driver = drive_nn;
    base_learner = all.learn;
    all.base_learn = all.learn;
    all.learn = learn;

    all.base_learner_nb_w *= k;
    increment = (all.length()/all.base_learner_nb_w) * all.stride;
    total_increment = increment*(k-1);

    bool initialize = true;

    memset (&output_layer, 0, sizeof (output_layer));
    push(output_layer.indices, nn_output_namespace);
    feature output = {1., nn_constant*all.stride};
    push(output_layer.atomics[nn_output_namespace], output);
    initialize &= (all.reg.weight_vectors[output_layer.atomics[nn_output_namespace][0].weight_index & all.weight_mask] == 0);

    for (unsigned int i = 0; i < k; ++i)
      {
        output.weight_index += all.stride;
        push(output_layer.atomics[nn_output_namespace], output);
        initialize &= (all.reg.weight_vectors[output_layer.atomics[nn_output_namespace][i+1].weight_index & all.weight_mask] == 0);
      }

    output_layer.num_features = k + 1;
    output_layer.in_use = true;

    if (initialize) {
      if (! all.quiet) 
        std::cerr << "randomly initializing neural network output weights and hidden bias" << std::endl;

      // output weights

      float sqrtk = sqrt ((double)k);
      for (unsigned int i = 0; i <= k; ++i)
        {
          weight* w = &all.reg.weight_vectors[output_layer.atomics[nn_output_namespace][i].weight_index & all.weight_mask];

          w[0] = (float) (drand48 () - 0.5) / sqrtk;

          // prevent divide by zero error
          if (dropout && all.normalized_updates)
            w[all.normalized_idx] = 1e-4;
        }

      // hidden biases

      unsigned int weight_index = constant * all.stride;

      for (unsigned int i = 0; i < k; ++i)
        {
          all.reg.weight_vectors[weight_index & all.weight_mask] = (float) (drand48 () - 0.5);
          weight_index += increment;
        }
    }

    squared_loss = getLossFunction (0, "squared", 0);

    atexit (free_output_layer);
    atexit (free_squared_loss);

    memset (xsubi, 0, sizeof (xsubi));
    unsigned short *old = seed48 (xsubi);
    memcpy (xsubi, old, sizeof (xsubi));
    memcpy (save_xsubi, old, sizeof (save_xsubi));
    seed48 (xsubi);
  }
}
