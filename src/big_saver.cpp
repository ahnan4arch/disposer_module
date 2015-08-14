//-----------------------------------------------------------------------------
// Copyright (c) 2015 Benjamin Buch
//
// https://github.com/bebuch/disposer
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt)
//-----------------------------------------------------------------------------
// TODO: add type in log messages
#include "big_saver.hpp"

#include "log.hpp"
#include "bitmap_sequence.hpp"
#include "name_generator.hpp"
#include "big_write.hpp"
#include "tar.hpp"

#include <disposer/module_base.hpp>


namespace disposer_module{ namespace big_saver{


	enum class input_t{
		sequence,
		vector,
		image
	};

	struct parameter{
		bool tar;

		std::size_t sequence_start;
		std::size_t camera_start;

		std::string dir;

		boost::optional< std::size_t > fixed_id;

		std::size_t sequence_count;

		input_t input;

		boost::optional< name_generator< std::size_t > > tar_pattern;
		boost::optional< name_generator< std::size_t, std::size_t, std::size_t > > big_pattern;
	};

	using save_image = boost::variant<
			std::reference_wrapper< bitmap< std::int8_t > const >,
			std::reference_wrapper< bitmap< std::uint8_t > const >,
			std::reference_wrapper< bitmap< std::int16_t > const >,
			std::reference_wrapper< bitmap< std::uint16_t > const >,
			std::reference_wrapper< bitmap< std::int32_t > const >,
			std::reference_wrapper< bitmap< std::uint32_t > const >,
			std::reference_wrapper< bitmap< std::int64_t > const >,
			std::reference_wrapper< bitmap< std::uint64_t > const >,
			std::reference_wrapper< bitmap< float > const >,
			std::reference_wrapper< bitmap< double > const >,
			std::reference_wrapper< bitmap< long double > const >
		>;

	using save_vector = std::vector< save_image >;

	using save_sequence = std::vector< save_vector >;


	struct module: disposer::module_base{
		module(std::string const& type, std::string const& chain, std::string const& name, parameter&& param):
			disposer::module_base(type, chain, name),
			param(std::move(param)){
				inputs = disposer::make_input_list(sequence, vector, image);
			}


		using types = disposer::type_list<
			std::int8_t,
			std::uint8_t,
			std::int16_t,
			std::uint16_t,
			std::int32_t,
			std::uint32_t,
			std::int64_t,
			std::uint64_t,
			float,
			double,
			long double
		>;


		disposer::container_input< bitmap_sequence, types > sequence{"sequence"};

		disposer::container_input< bitmap_vector, types > vector{"vector"};

		disposer::container_input< bitmap, types > image{"image"};


		void save(std::size_t id, save_sequence const& bitmap_sequence);


		std::string tar_name(std::size_t id);
		std::string big_name(std::size_t cam, std::size_t pos);
		std::string big_name(std::size_t id, std::size_t cam, std::size_t pos);


		template < typename T >
		void trigger_sequence(std::size_t id);

		template < typename T >
		void trigger_vector(std::size_t id);

		template < typename T >
		void trigger_image(std::size_t id);


		void trigger(std::size_t id)override;


		parameter const param;
	};


	disposer::module_ptr make_module(
		std::string const& type,
		std::string const& chain,
		std::string const& name,
		disposer::io_list const& inputs,
		disposer::io_list const&,
		disposer::parameter_processor& params,
		bool is_start
	){
		if(is_start) throw disposer::module_not_as_start(type, chain);

		std::array< bool, 3 > const use_input{{
			inputs.find("sequence") != inputs.end(),
			inputs.find("vector") != inputs.end(),
			inputs.find("image") != inputs.end()
		}};

		std::size_t input_count = std::count(use_input.begin(), use_input.end(), true);

		if(input_count > 1){
			throw std::logic_error(type + ": Can only use one input ('image', 'vector' or 'sequence')");
		}

		if(input_count == 0){
			throw std::logic_error(type + ": No input defined (use 'image', 'vector' or 'sequence')");
		}

		parameter param;

		params.set(param.tar, "tar", false);

		params.set(param.sequence_start, "sequence_start", 0);
		params.set(param.camera_start, "camera_start", 0);

		params.set(param.dir, "dir", ".");

		std::size_t id_digits;
		std::size_t camera_digits;
		std::size_t position_digits;
		params.set(id_digits, "id_digits", 3);
		params.set(camera_digits, "camera_digits", 1);
		params.set(position_digits, "position_digits", 3);

		params.set(param.fixed_id, "fixed_id");

		if(use_input[0]){
			param.input = input_t::sequence;
		}else if(use_input[1]){
			param.input = input_t::vector;
		}else{
			param.input = input_t::image;
			params.set(param.sequence_count, "sequence_count");
			if(param.sequence_count == 0){
				throw std::logic_error(make_string(type + ": sequence_count (value: ", param.sequence_count, ") needs to be greater than 0"));
			}
		}

		struct format{
			std::size_t digits;

			std::string operator()(std::size_t value){
				std::ostringstream os;
				os << std::setw(digits) << std::setfill('0') << value;
				return os.str();
			}
		};

		if(param.tar){
			param.tar_pattern = make_name_generator(
				params.get< std::string >("tar_pattern", "${id}.tar"),
				{{true}},
				std::make_pair("id", format{id_digits})
			);
		}

		param.big_pattern = make_name_generator(
			params.get("big_pattern", param.tar ? std::string("${cam}_${pos}.big") : std::string("${id}_${cam}_${pos}.big")),
			{{!param.tar, true, true}},
			std::make_pair("id", format{id_digits}),
			std::make_pair("cam", format{camera_digits}),
			std::make_pair("pos", format{position_digits})
		);

		return std::make_unique< module >(type, chain, name, std::move(param));
	}

	void init(){
		add_module_maker("big_saver", &make_module);
	}


	struct big_stream_visitor: boost::static_visitor< void >{
		big_stream_visitor(std::ostream& os): os(os){}

		std::ostream& os;

		template < typename T >
		void operator()(T const& image_ref)const{
			big::write(image_ref.get(), os);
		}
	};

	struct big_file_visitor: boost::static_visitor< void >{
		big_file_visitor(std::string const& name): name(name){}

		std::string const& name;

		template < typename T >
		void operator()(T const& image_ref)const{
			big::write(image_ref.get(), name);
		}
	};


	void module::save(std::size_t id, save_sequence const& bitmap_sequence){
		auto used_id = param.fixed_id ? *param.fixed_id : id;

		if(param.tar){
			auto tarname = param.dir + "/" + (*param.tar_pattern)(used_id);
			disposer::log([this, &tarname, id](log::info& os){ os << type << " id " << id << ": write '" << tarname << "'"; }, [this, id, used_id, &bitmap_sequence, &tarname]{
				tar_writer tar(tarname);
				std::size_t cam = param.camera_start;
				for(auto& sequence: bitmap_sequence){
					std::size_t pos = param.sequence_start;
					for(auto& bitmap: sequence){
						auto filename = (*param.big_pattern)(used_id, cam, pos);
						disposer::log([this, &tarname, &filename, id](log::info& os){ os << type << " id " << id << ": write '" << tarname << "/" << filename << "'"; }, [&tar, &bitmap, &filename]{
							tar.write(filename, [&bitmap](std::ostream& os){
								boost::apply_visitor(big_stream_visitor(os), bitmap);
							});
						});
						++pos;
					}
					++cam;
				}
			});
		}else{
			std::size_t cam = param.camera_start;
			for(auto& sequence: bitmap_sequence){
				std::size_t pos = param.sequence_start;
				for(auto& bitmap: sequence){
					auto filename = param.dir + "/" + (*param.big_pattern)(used_id, cam, pos);
					disposer::log([this, &filename, id](log::info& os){ os << type << " id " << id << ": write '" << filename << "'"; }, [&bitmap, &filename]{
						boost::apply_visitor(big_file_visitor(filename), bitmap);
					});
					++pos;
				}
				++cam;
			}
		}
	}


	struct sequence_visitor: boost::static_visitor< save_sequence >{
		sequence_visitor(std::size_t id): id(id){}

		std::size_t const id;

		template < typename T >
		save_sequence operator()(T const& sequence)const{
			save_sequence data;
			for(auto& vector: sequence.data()){
				data.emplace_back();
				for(auto& bitmap: vector){
					data.back().emplace_back(bitmap);
				}
			}

			return data;
		}
	};

	struct vector_visitor: boost::static_visitor< save_vector >{
		template < typename T >
		save_vector operator()(T const& vectors)const{
			save_vector result;
			for(auto& image: vectors.data()){
				result.emplace_back(image);
			}

			return result;
		}
	};

	struct image_visitor: boost::static_visitor< save_image >{
		template < typename T >
		save_image operator()(T const& image)const{
			return image.data();
		}
	};


	void module::trigger(std::size_t id){
		switch(param.input){
			case input_t::sequence:{
				for(auto const& pair: sequence.get(id)){
					auto id = pair.first;
					auto& bitmap_sequence = pair.second;

					save(id, boost::apply_visitor(sequence_visitor(id), bitmap_sequence));
				}
			}break;
			case input_t::vector:{
				auto vectors = vector.get(id);
				auto from = vectors.begin();

				while(from != vectors.end()){
					auto id = from->first;
					auto to = vectors.upper_bound(id);

					save_sequence data;
					for(auto iter = from; iter != to; ++iter){
						data.emplace_back(boost::apply_visitor(vector_visitor(), iter->second));
					}

					save(id, data);

					from = to;
				}
			}break;
			case input_t::image:{
				auto images = image.get(id);
				auto from = images.begin();

				while(from != images.end()){
					auto id = from->first;
					auto to = images.upper_bound(id);

					std::size_t pos = 0;
					save_sequence data;
					for(auto iter = from; iter != to; ++iter){
						if(pos == 0) data.emplace_back();
						++pos;

						if(pos == param.sequence_count) pos = 0;

						data.back().emplace_back(boost::apply_visitor(image_visitor(), iter->second));
					}

					if(pos != 0){
						throw std::runtime_error(type + ": single image count does not match parameter 'sequence_count'");
					}

					save(id, data);

					from = to;
				}
			}break;
		}
	}


} }
